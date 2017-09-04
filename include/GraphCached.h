#ifndef _GRAPH_CACHED_H_
#define _GRAPH_CACHED_H_

#include <string>
#include <atomic>
#include <iostream>
#include <thread>
#include <map>
#include <tuple>

#include "DiskComponent.h"
#include "CacheManager.h"
#include "queue.hpp"
#include "debug.h"
// GraphCached user code
// class Graph : public GraphCached<int> {
//
// };

// GraphCached code

#define COLLECT 

namespace graphcached {


template <class KeyTy, class ValueTy>
class GraphCached {
protected:
	std::string baseFilename;
	CacheManager<KeyTy, ValueTy>* cachemanager;         // manage the memory cache space
        // dump mutex
        std::mutex dMutex;
        Queue<KeyTy>* reqQ;
	Queue<ValueTy*>* readyQ;
	Queue<DiskComponent<KeyTy>*>* releaseQ;
        Queue<size_t>* hintQ;
	Queue<DiskComponent<KeyTy>*>* ioQ;
	std::thread* cacheap;
	std::thread* iothread;
#ifdef COLLECT
	std::atomic<long> rtimes;
	std::atomic<long> rhits;
	std::atomic<long> wtimes;
	std::atomic<long> whits;
	std::atomic<long long int> mbytes;

        std::atomic<long> brtimes;
	std::atomic<long> brhits;
#endif
#ifdef DEBUG
        std::list<std::tuple<double, KeyTy, size_t>> ioStart;
	std::list<std::tuple<double, KeyTy, size_t>> ioEnd;
	std::list<std::pair<double, size_t>> runHint;
	std::list<std::pair<double, KeyTy>> runRelease;
	std::list<std::pair<double, KeyTy>> runHit;
	std::list<std::pair<double, KeyTy>> runMiss;
        std::list<std::tuple<double, KeyTy, size_t>> runPHit;
        double startTime;
#endif
public:
        GraphCached(){
#ifdef COLLECT
            brtimes = brhits = mbytes = rtimes = rhits = wtimes = whits = 0;
#endif
	    cachemanager = new CacheManager<KeyTy, ValueTy>(12u, 4096*1024*1024ull, 24*1024*1024u);
	}
	GraphCached(std::string filename): baseFilename(filename)   {
#ifdef COLLECT
            brtimes = brhits = mbytes = rtimes = rhits = wtimes = whits = 0;
#endif
	    cachemanager = new CacheManager<KeyTy, ValueTy>(12u, 4096*1024*1024ull, 24*1024*1024u);
	}

	GraphCached(std::string filename, uint32_t clsp, uint64_t cs, uint64_t mps): baseFilename(filename) {
#ifdef COLLECT
            brtimes = brhits = mbytes = rtimes = rhits = wtimes = whits = 0;
#endif
	    cachemanager = new CacheManager<KeyTy, ValueTy>(clsp, cs, mps);
	}
	~GraphCached(){}
        

	int startCacheap() {
	    reqQ = new Queue<KeyTy>(65536);
	    readyQ = new Queue<ValueTy*>(65536);
	    releaseQ = new Queue<DiskComponent<KeyTy>*>(65536);
	    hintQ = new Queue<size_t>(65536);
	    ioQ = new Queue<DiskComponent<KeyTy>*>(131072);
	    iothread = new std::thread(&GraphCached<KeyTy, ValueTy>::io, this);
	    cacheap = new std::thread(&GraphCached<KeyTy, ValueTy>::run, this);
	    D(startTime = get_time();)
	    return 0;
	}
	virtual DiskSegmentInfo plocate(KeyTy key) = 0;
	void io() {
#ifdef COLLECT
    //rtimes++;
        auto cacheLineSize = cachemanager->getCacheLineSize();
#endif
        D(int i = 0;)
	    while(1) {
	        auto it = ioQ->front();
		if (it == nullptr) {
		    readyQ->push(nullptr);
            ioQ->pop();
		    continue;
		}
		auto dsi = it->dsi;
		D(ioStart.push_back(std::make_tuple(get_time() - startTime, it->gkey, it->dsi._size));)
        //D(std::cout<<"ioStart"<<std::endl;)
	        if (it->state == -2) {
                auto ret = cachemanager->cache(dsi._size, it->gkey, it);
                if (ret == nullptr) continue;
	            //D(std::cout<<"cached"<<std::endl;)
                it = ret;
                int fd = dsi._fd; 
	            size_t bytes = 0;
	            while (bytes < dsi._size) {
	                size_t size = dsi._size;
	                if ((dsi._size&PAGESIZEMASK2) != 0) {
	                    size = (dsi._size & PAGESIZEMASK1) + GCPAGESIZE;
	                }
	                size_t cur = pread(fd, it->addr, size, dsi._offset);
		        if (cur == -1) { std::cout<<"read file error"<<std::endl; perror("read file error:"); exit(0);}
		        bytes += cur;
	            }
	        // change state
	            it->curSize = it->size;
	            it->state = 1;
#ifdef COLLECT
                brtimes += it->size / cacheLineSize;
#endif
	            readyQ->push(reinterpret_cast<ValueTy*>(it));
	            //D(std::cout<<"ready:"<<++i<<std::endl;)
                ioQ->pop();
            }
		else if (it->state == -1) {
            auto ret = cachemanager->recache(it);
            if (ret == nullptr) continue;
		    it = ret;
            uint64_t size = it->size - it->curSize;
	            int fd = dsi._fd;
	            size_t bytes = 0;
	            while (bytes < size) {
	                 size_t cur = pread(fd, reinterpret_cast<char*>(it->addr)+it->curSize, size, dsi._offset+it->curSize);
                         if (cur == -1) { std::cout<<"key:"<<std::get<0>(it->gkey)<<" "<<std::get<1>(it->gkey)<<" it->size:"<<it->size<<" it->curSize:"<<it->curSize<<" fd:"<<fd<<" addr:"<<it->curSize<<" size:"<<size<<" offset:"<<dsi._offset+it->curSize<<std::endl; perror("read file error:"); exit(0);}
	                 bytes += cur;
	             }
#ifdef COLLECT
                     mbytes += bytes;
#endif
	     // set the state
	             it->curSize = it->size;
	             it->state = 1;
	             readyQ->push(reinterpret_cast<ValueTy*>(it));
	        ioQ->pop();    
        }
		else {std::cout<<"state error"<<std::endl;}
		D(ioEnd.push_back(std::make_tuple(get_time() - startTime, it->gkey, it->dsi._size));)
        //D(std::cout<<"ioEnd"<<std::endl;)
	    }
        }

	void run() {
#ifdef COLLECT
    //rtimes++;
    auto cacheLineSize = cachemanager->getCacheLineSize();
#endif
        D(int i = 0;)
	    while (1) {
	        while(!hintQ->is_empty()) {
		    size_t pid = hintQ->pop();
		    cachemanager->reorder(pid);
		    D(runHint.push_back(std::make_pair(getTime() - startTime, pid));)
		    //D(std::cout<<"hint: "<<pid<<std::endl;)
            }
		    int times = 0;
		    while (!releaseQ->is_empty() && times < 10) {
		        auto dc = releaseQ->pop();
	            if (dc == nullptr) {
		            cachemanager->endIter();
			        break;
		        }
		        inner_release(dc);
		        D(runRelease.push_back(std::make_pair(getTime() - startTime, dc->gkey));)
		        //D(std::cout<<"release"<<std::endl;)
                times++;
		    }
	        while(!reqQ->is_empty()) {
            KeyTy key = reqQ->front();
		    if (std::get<0>(key) == -1) {
		        reqQ->pop();
		    //readyQ->push(nullptr);
		        ioQ->push(nullptr);
		        continue;
		    }
		    auto dsi = plocate(key);
		    auto it = cachemanager->find(key); 
		    if (it) {

	        if (it->state >= 0) {
#ifdef COLLECT
                brhits += it->size / cacheLineSize;
#endif
#ifdef COLLECT
	            brtimes += it->size / cacheLineSize;
#endif
		        reqQ->pop();
		        readyQ->push(it);
		        D(runHit.push_back(std::make_pair(get_time() - startTime, key));)
		        //D(std::cout<<"hit"<<std::endl;)
			    continue;
	        }
	        if (it->state<0) { // find enough space to load the the evicted part
    	        if (it->state != -1) {std::cerr<<"error"<<std::endl; exit(0);};
	                //cachemanager->recache(it);
			//if (it == nullptr) {
			//    continue;
			//}
			    D(runPHit.push_back(std::make_tuple(get_time() - startTime, it->gkey, it->dsi._size));)
		        //D(std::cout<<"phit"<<std::endl;)
#ifdef COLLECT
                brhits += it->curSize / cacheLineSize;
#endif
#ifdef COLLECT
	            brtimes += it->size / cacheLineSize;
#endif
	            ioQ->push(it);
	        }
            }
    // else, read the item from disk and potentially store it into the memory cached depending on the 'cachedFlag' (which is the last parameter in this funtion)
            else {
                  //auto nit = cachemanager->cache(dsi._size, key);
	              //if (nit == nullptr) {
	                  //continue;
	              // }
             auto nit = reinterpret_cast<DiskComponent<KeyTy>*>(new ValueTy());
                  
	         nit->dsi = dsi;
	         nit->gkey = key;
		     D(runMiss.push_back(std::make_pair(get_time() - startTime, key));)
		     //D(std::cout<<"miss:"<<++i<<std::endl;)
#ifdef COLLECT
             mbytes += dsi._size;
#endif
	         ioQ->push(nit);
             }
	         reqQ->pop();
            }
	     }
    }

	ValueTy* get() {
	    //D(std::cout<<"get"<<std::endl;)
        return readyQ->pop();
	}
	void request(KeyTy key) {
	    reqQ->push(key);
	}
	void release(DiskComponent<KeyTy>* dc) {
	    releaseQ->push(dc);
	}

	int inner_release(DiskComponent<KeyTy>* key);
	ValueTy* read(KeyTy key, int cacheFlag = 1);
	int write(KeyTy key, ValueTy& value, int wbFlag = 0);
	//virtual int readFromFile(KeyTy& key, ValueTy& value) = 0;
	//virtual int writeToFile(KeyTy& key, ValueTy& value) = 0;
	void dump() {
	    std::lock_guard<std::mutex> dLock(dMutex);
	    cachemanager->dump();
	}
	D(void dumpEvents() {
	    char tmp[64];
	    int fevents = open("/home/zhaopeng/graph/GG-GraphCached/events.dat", O_CREAT|O_WRONLY|O_APPEND, 0600);
	    std::cout<<"file created"<<std::endl;
	    for (auto it = ioStart.begin(); it != ioStart.end(); ++it) {
	        auto second = std::get<1>(*it);
	        int len = sprintf(tmp, "ioStart %lf %d:%d:%d %lu\n", std::get<0>(*it), std::get<0>(second), std::get<1>(second), std::get<2>(second), std::get<2>(*it));
		::write(fevents, tmp, len);
	    }
	    for (auto it = ioEnd.begin(); it != ioEnd.end(); ++it) {
	        auto second = std::get<1>(*it);
	        int len = sprintf(tmp, "ioEnd %lf %d:%d:%d %lu\n", std::get<0>(*it), std::get<0>(second), std::get<1>(second), std::get<2>(second), std::get<2>(*it));
		::write(fevents, tmp, len);
	    }
	    for (auto it = runHint.begin(); it != runHint.end(); ++it) {
	        auto second = (*it).second;
	        int len = sprintf(tmp, "runHint %lf %d\n", (*it).first, second);
		::write(fevents, tmp, len);
	    }
	    for (auto it = runHit.begin(); it != runHit.end(); ++it) {
	        auto second = (*it).second;
	        int len = sprintf(tmp, "runHit %lf %d:%d:%d\n", (*it).first, std::get<0>(second), std::get<1>(second), std::get<2>(second));
		::write(fevents, tmp, len);
	    }
	    for (auto it = runMiss.begin(); it != runMiss.end(); ++it) {
	        auto second = (*it).second;
	        int len = sprintf(tmp, "runMiss %lf %d:%d:%d\n", (*it).first, std::get<0>(second), std::get<1>(second), std::get<2>(second));
		::write(fevents, tmp, len);
	    }
	    for (auto it = runPHit.begin(); it != runPHit.end(); ++it) {
	        auto second = std::get<1>(*it);
	        int len = sprintf(tmp, "runPHit %lf %d:%d:%d %lu\n", std::get<0>(*it), std::get<0>(second), std::get<1>(second), std::get<2>(second), std::get<2>(*it));
		::write(fevents, tmp, len);
	    }
	    close(fevents);
	})
	void stat() {
#ifdef COLLECT
            //std::cout<<"total partition read times: "<<rtimes<<std::endl;
            //std::cout<<"total partition read hits: "<<rhits<<std::endl;
            //std::cout<<"total partition write times: "<<wtimes<<std::endl;
            //std::cout<<"total partition write hits: "<<whits<<std::endl;
            //std::cout<<"partition hit ratio: "<<(rhits+whits)/(1.0*(rtimes+wtimes))<<std::endl;
            std::cout<<"miss bytes: "<<mbytes<<std::endl<<std::endl;
            std::cout<<"total block read times: "<<brtimes<<std::endl;
            std::cout<<"total block read hits: "<<brhits<<std::endl;
	    std::cout<<"block hit ratio: "<<(brhits)/(1.0*brtimes)<<std::endl;
            std::cout<<"mmap count: "<<cachemanager->getMmapcount()<<std::endl;
	    std::cout<<"mremap count: "<<cachemanager->getMremapcount()<<std::endl;
	    std::cout<<"munmap count: "<<cachemanager->getMunmapcount()<<std::endl;
	    //D(dumpEvents();)
#endif
#ifdef LRURETRY
            print_nretry();
#endif
	}
};
template <class KeyTy, class ValueTy>
int GraphCached<KeyTy, ValueTy>::inner_release(DiskComponent<KeyTy>* dc){
            dc->refcount--;
	    //int expected = 1;
	    //int desired = 0;
	    //int rc = dc->refcount;
	    //if (rc == 0 && dc->state.compare_exchange_strong(expected, desired)) {
	    //    cachemanager->release(dc);
	    //}
	    dc->state = 0;
	    cachemanager->release(dc);
	    return 0;
}
const int UPDATE_LRU = 1;
template <class KeyTy, class ValueTy>
ValueTy* GraphCached<KeyTy, ValueTy>::read(KeyTy key, int cacheFlag /* default = 1*/) {
    // fisrt, try to get the item from the memory cache
#ifdef COLLECT
    //rtimes++;
    auto cacheLineSize = cachemanager->getCacheLineSize();
#endif
    // dump();
    auto it = cachemanager->find(key); 
    return nullptr;
}
template <class KeyTy, class ValueTy>
int GraphCached<KeyTy, ValueTy>::write(KeyTy key, ValueTy& value, int wbFlag /* default = 0*/) {

}
} // namespace graphcached

#endif
