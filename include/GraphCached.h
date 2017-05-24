#ifndef _GRAPH_CACHED_H_
#define _GRAPH_CACHED_H_

#include <string>
#include <atomic>
#include <iostream>

// make functions in memcached.h have 'C' linkage
extern "C" {
#include "memcached.h"
}
// GraphCached user code
// class Graph : public GraphCached<int> {
//
// };

// GraphCached code

#define COLLECT 

namespace graphcached {

static std::atomic<int> initialized(0);
int GraphCached_init(int argc, char* argv[]) {
    if (initialized == 0) {
        init_memcached(argc, argv);
	initialized = 1;
    }
    else {
        std::cerr<<"Error: multiple initialization of GraphCached!"<<std::endl;
	return 1;
    }
    return 0;
}

template<class KeyTy, class ValueTy> class GraphCached;
using GraphCachedKey = std::string; 

class DiskSegmentInfo {
public:
	std::string _filename;
	off_t _offset;
	size_t _size;
public:
	DiskSegmentInfo(std::string fn, off_t off, size_t sz) : _filename(fn), _offset(off), _size(sz){}
	DiskSegmentInfo(): _filename(""), _offset(0), _size(0) {}
	size_t size() {return _size;}
};

class DiskComponent {
public:
	DiskSegmentInfo dsi;
	GraphCachedKey gkey;
	item* pitem;
	union {
	    uint64_t cas;
	    char end;
	} data[];
	
public:
	DiskComponent() {}
	DiskComponent(GraphCachedKey key): gkey(key) {}
	~DiskComponent() {}
	GraphCachedKey& getKey() {return gkey;}
	virtual size_t getSize() {return dsi.size() + sizeof(dsi) + gkey.size();}
	int release() {
	    if (pitem) { 
	        do_item_remove(pitem);
		return 1;
	    }
	    return 0;
	}
};

template <class KeyTy, class ValueTy>
class GraphCached {
protected:
	std::string baseFilename;
#ifdef COLLECT
	std::atomic<long> rtimes;
	std::atomic<long> rhits;
	std::atomic<long> wtimes;
	std::atomic<long> whits;
#endif
public:
        GraphCached(){
#ifdef COLLECT
            rtimes = rhits = wtimes = whits = 0;
#endif
	}
	GraphCached(std::string filename): baseFilename(filename) {
#ifdef COLLECT
            rtimes = rhits = wtimes = whits = 0;
#endif 
	}
	~GraphCached(){}
	ValueTy* read(KeyTy key, int cacheFlag = 1);
	int write(KeyTy key, ValueTy& value, int wbFlag = 0);
	//virtual int readFromFile(KeyTy& key, ValueTy& value) = 0;
	//virtual int writeToFile(KeyTy& key, ValueTy& value) = 0;
	virtual DiskSegmentInfo plocate(KeyTy key) = 0;
	void stat() {
#ifdef COLLECT
            std::cout<<"total read times: "<<rtimes<<std::endl;
            std::cout<<"total read hits: "<<rhits<<std::endl;
            std::cout<<"total write times: "<<wtimes<<std::endl;
            std::cout<<"total write hits: "<<whits<<std::endl;
            std::cout<<"hit rate: "<<(rhits+whits)/(1.0*(rtimes+wtimes))<<std::endl;
#endif
	}
};

const int UPDATE_LRU = 1;

template <class KeyTy, class ValueTy>
ValueTy* GraphCached<KeyTy, ValueTy>::read(KeyTy key, int cacheFlag /* default = 1*/) {
    // fisrt, try to get the item from the memory cache
#ifdef COLLECT
    rtimes++;
#endif
    char* dckey = reinterpret_cast<char*>(&key);
    size_t ndckey = sizeof(key);
    item* it;
    uint32_t hv;
    hv = hash(dckey, ndckey);
    item_lock(hv);
    it = do_item_get(dckey, ndckey, hv, NULL, UPDATE_LRU);
    item_unlock(hv);
    
    // if the item is not null, mark it as a hit and return it
    if (it != NULL) {
#ifdef COLLECT
        rhits++;
#endif
        //std::cout<<"hit"<<std::endl;
        //do_item_remove(it);
	return reinterpret_cast<ValueTy*>(ITEM_data(it));  
    }
    // else, read the item from disk and potentially store it into the memory cached depending on the 'cachedFlag' (which is the last parameter in this funtion)
    else {
       DiskSegmentInfo dsi = plocate(key);
       if (cacheFlag) {
           //DiskSegmentInfo dsi = plocate(key);
	   //std::cout<<"dsi._size = "<<dsi.size()<<" sizeof(ValueTy) = "<<sizeof(ValueTy)<<std::endl;
	   it = item_alloc(dckey, ndckey, 0, 0/*expiration time, 0 means never expire*/, dsi.size()+ sizeof(ValueTy));
	   if (it != NULL) {
	       // use placement new to place the new object at the allocated address
	       DiskComponent* value = new(static_cast<void*>(ITEM_data(it))) ValueTy();
	       //printf("item data address: %p\n", ITEM_data(it));
	       //printf("value address: %p\n", value);
	       //printf("dsi._size = %d, dsi._off = %d\n", dsi._size, dsi._offset);
	       value->dsi = dsi;
	       value->gkey = dckey;
	       value->pitem = it;
	       int fd = open(dsi._filename.c_str(), O_RDONLY);
	       if (fd == -1)
	           std::cerr<< "Cannot open file: "<< dsi._filename<< std::endl;
	       size_t bytes = 0;
	       while (bytes < dsi._size) {
	           size_t cur = pread(fd, value->data, dsi._size - bytes, dsi._offset + bytes);
		   bytes += cur;
	       }
	       //printf("first line: ");
	       //char* data = reinterpret_cast<char*>(value->data);
	       //int i = 0;
	       //while ('\n' != data[i]) 
	       //    printf("%c", data[i++]);
	       //printf("\n");
	       
	       close(fd);
	       // the function do_item_link() will add the item into memory cache and increase its ref counter by 1
	       do_item_link(it, hv);
	       // decrease the ref counter before exit
	       //do_item_remove(it);
	       return dynamic_cast<ValueTy*>(value);
	   }
	   else {
	       //fprintf(stderr, "allocation error!\n");
	       //return NULL;
	       // just do not cache the item !
	   }
       }
       {
           char* tmpbuffer = new char[sizeof(ValueTy)+dsi._size];
	   DiskComponent * value = new(static_cast<void*>(tmpbuffer)) ValueTy();
	   value->dsi = dsi;
	   value->gkey = dckey;
	   value->pitem = NULL;
	   int fd = open(dsi._filename.c_str(), O_RDONLY);
	   size_t bytes = 0;
	   while (bytes < dsi._size) {
	       size_t cur = pread(fd, value->data, dsi._size - bytes, dsi._offset + bytes);
	       bytes += cur;
	   }
	   close(fd);
           return dynamic_cast<ValueTy*>(value);
        }
    }
}
template <class KeyTy, class ValueTy>
int GraphCached<KeyTy, ValueTy>::write(KeyTy key, ValueTy& value, int wbFlag /* default = 0*/) {

}
} // namespace graphcached

#endif
