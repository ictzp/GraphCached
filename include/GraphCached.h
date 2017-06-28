#ifndef _GRAPH_CACHED_H_
#define _GRAPH_CACHED_H_

#include <string>
#include <atomic>
#include <iostream>

#include "DiskComponent.h"
#include "CacheManager.h"
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

#ifdef COLLECT
	std::atomic<long> rtimes;
	std::atomic<long> rhits;
	std::atomic<long> wtimes;
	std::atomic<long> whits;
	std::atomic<long long int> mbytes;

        std::atomic<long> brtimes;
	std::atomic<long> brhits;
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
	
	int release(DiskComponent<KeyTy>* key);
	ValueTy* read(KeyTy key, int cacheFlag = 1);
	int write(KeyTy key, ValueTy& value, int wbFlag = 0);
	//virtual int readFromFile(KeyTy& key, ValueTy& value) = 0;
	//virtual int writeToFile(KeyTy& key, ValueTy& value) = 0;
	virtual DiskSegmentInfo plocate(KeyTy key) = 0;
	void dump() {
	    std::lock_guard<std::mutex> dLock(dMutex);
	    cachemanager->dump();
	}
	void stat() {
#ifdef COLLECT
            std::cout<<"total partition read times: "<<rtimes<<std::endl;
            std::cout<<"total partition read hits: "<<rhits<<std::endl;
            std::cout<<"total partition write times: "<<wtimes<<std::endl;
            std::cout<<"total partition write hits: "<<whits<<std::endl;
            std::cout<<"partition hit ratio: "<<(rhits+whits)/(1.0*(rtimes+wtimes))<<std::endl;
            std::cout<<"miss bytes: "<<mbytes<<std::endl<<std::endl;
            std::cout<<"total block read times: "<<brtimes<<std::endl;
            std::cout<<"total block read hits: "<<brhits<<std::endl;
	    std::cout<<"block hit ratio: "<<(brhits)/(1.0*brtimes)<<std::endl;
#endif
#ifdef LRURETRY
            print_nretry();
#endif
	}
};
template <class KeyTy, class ValueTy>
int GraphCached<KeyTy, ValueTy>::release(DiskComponent<KeyTy>* dc){
            dc->refcount--;
	    int expected = 1;
	    int desired = 0;
	    int rc = dc->refcount;
	    if (rc == 0 && dc->state.compare_exchange_strong(expected, desired)) {
		cachemanager->release(dc);
	    }
	    return 0;
}
const int UPDATE_LRU = 1;
template <class KeyTy, class ValueTy>
ValueTy* GraphCached<KeyTy, ValueTy>::read(KeyTy key, int cacheFlag /* default = 1*/) {
    // fisrt, try to get the item from the memory cache
#ifdef COLLECT
    rtimes++;
    auto cacheLineSize = cachemanager->getCacheLineSize();
#endif
    // dump();
    auto it = cachemanager->find(key); 
    DiskSegmentInfo dsi = plocate(key);
    // if the item is not null, mark it as a hit and return it
    if (it != NULL) {
#ifdef COLLECT
        rhits++;
	brtimes += it->size / cacheLineSize;
#endif

	if (it->state >= 0) {
#ifdef COLLECT
            brhits += it->size / cacheLineSize;
#endif
	    return reinterpret_cast<ValueTy*>(it); 
	}
	else { // find enough space to load the the evicted part
#ifdef COLLECT
            brhits += it->curSize / cacheLineSize;
#endif
	    cachemanager->recache(it);
	    uint64_t size = it->size - it->curSize;
	    int fd = dsi._fd;
	    size_t bytes = 0;
	    while (bytes < size) {
	        size_t cur = pread(fd, reinterpret_cast<char*>(it->addr)+it->curSize, size, dsi._offset+it->curSize);
                if (cur == -1) { std::cout<<"read file error"<<std::endl; exit(0);}
	        bytes += cur;
	     }
#ifdef COLLECT
                mbytes += bytes;
#endif
	     // set the state
	     it->curSize = it->size;
	     it->state = 1;
	     return reinterpret_cast<ValueTy*>(it);
	 }
    }
    // else, read the item from disk and potentially store it into the memory cached depending on the 'cachedFlag' (which is the last parameter in this funtion)
    else {
#ifdef COLLECT
       mbytes += dsi._size;
#endif
       if (cacheFlag) {
           auto nit = cachemanager->cache(dsi._size, key);
	   if (nit == nullptr) {
	       std::cerr<<"no enough memory."<<std::endl;
	       exit(0);
	   }
	   nit->dsi = dsi;
	   nit->gkey = key;
	   int fd = dsi._fd; 
	   size_t bytes = 0;
	   while (bytes < dsi._size) {
	       size_t size = dsi._size;
	       if ((dsi._size&PAGESIZEMASK2) != 0) {
	           size = (dsi._size & PAGESIZEMASK1) + GCPAGESIZE;
	        }
	        size_t cur = pread(fd, nit->addr, size, dsi._offset);
		if (cur == -1) { std::cout<<"read file error"<<std::endl; exit(0);}
		bytes += cur;
	   }
	   // change state
	   nit->curSize = nit->size;
	   nit->state = 1;
#ifdef COLLECT
           brtimes += nit->size / cachemanager->getCacheLineSize();
#endif
	   return reinterpret_cast<ValueTy*>(nit);
        }
    }
}
template <class KeyTy, class ValueTy>
int GraphCached<KeyTy, ValueTy>::write(KeyTy key, ValueTy& value, int wbFlag /* default = 0*/) {

}
} // namespace graphcached

#endif
