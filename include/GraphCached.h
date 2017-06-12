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
	int _fd;
	off_t _offset;
	size_t _size;
public:
	DiskSegmentInfo(int fn, off_t off, size_t sz) : _fd(fn), _offset(off), _size(sz){}
	DiskSegmentInfo(): _fd(0), _offset(0), _size(0) {}
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
	} _data[];
	
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
	// return aligned data address
	void* data() {
	   auto ptr = reinterpret_cast<std::uintptr_t>(_data);
	   auto tmp = ptr & PAGESIZEMASK1;
	   return reinterpret_cast<void*>(tmp + ((ptr & PAGESIZEMASK2) == 0 ? 0 : GCPAGESIZE));
	}
};

class Item;
class Hashtable;
class LruList;

template <class KeyTy, class ValueTy>
class GraphCached {
protected:
	std::string baseFilename;
        //Hashtable<KeyTy> hashtable;   // cache hashtable
        //LruList lrulist;         // cache lru list
	//CacheManager cachemanager;         // manage the memory cache space
#ifdef COLLECT
	std::atomic<long> rtimes;
	std::atomic<long> rhits;
	std::atomic<long> wtimes;
	std::atomic<long> whits;
	std::atomic<long long int> mbytes;
#endif
public:
        GraphCached(){
#ifdef COLLECT
            mbytes = rtimes = rhits = wtimes = whits = 0;
#endif
	}
	GraphCached(std::string filename): baseFilename(filename)   {
#ifdef COLLECT
            mbytes = rtimes = rhits = wtimes = whits = 0;
#endif
	}
	// GraphCached(std::string filename, size_t cls, size_t cs, size_t mps): baseFilename(filename)   {
#ifdef C// OLLECT
        //     mbytes = rtimes = rhits = wtimes = whits = 0;
#endif
        //     initGraphCache(cls, cs, mps);
        // }
	~GraphCached(){}
	
	// void initGraphCache(size_t cls, size_t cs, size_t mps)
	// {
	//     hashtable = new Hashtable();
	//     lruList = new LruList();
	//     cachemanager = new CacheManager(cls, cs, mps);
	// }
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
            std::cout<<"miss bytes: "<<mbytes<<std::endl;
#endif
#ifdef LRURETRY
            print_nretry();
#endif
	}
};

const int UPDATE_LRU = 1;
void print_key(const char* key, size_t nkey) {
    assert(nkey > 0);
    int i;
    printf("0x");
    for (i = 0; i < nkey - 1; i++) {
        printf("%02x-", key[i]);
    }
    printf("%02x", key[i]);
}

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
        //printf("key = ");
        //print_key(dckey, ndckey);
        //printf(" hv = %u\n", hv);
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
#ifdef COLLECT
       mbytes += dsi._size;
#endif
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
	       int fd = dsi._fd; //open(dsi._filename.c_str(), O_RDONLY|O_DIRECT);
	       //if (fd == -1)
	       //    std::cerr<< "Cannot open file: "<< dsi._filename<< std::endl;
	       size_t bytes = 0;
	       while (bytes < dsi._size) {
	           size_t size = dsi._size;
	           if ((dsi._size&PAGESIZEMASK2) != 0) {
	               size = (dsi._size & PAGESIZEMASK1) + GCPAGESIZE;
		   }
	           size_t cur = pread(fd, value->data(), size, dsi._offset);
		   if (cur == -1) { std::cout<<"read file error"<<std::endl; exit(0);}
		   bytes += cur;
	       }
	       //printf("first line: ");
	       //char* data = reinterpret_cast<char*>(value->data);
	       //int i = 0;
	       //while ('\n' != data[i]) 
	       //    printf("%c", data[i++]);
	       //printf("\n");
	       
	       //close(fd);
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
	   char* tmpbuffer = new char[sizeof(ValueTy) + dsi._size + GCPAGESIZE - 1];
	   DiskComponent * value = new(static_cast<void*>(tmpbuffer)) ValueTy();
	   value->dsi = dsi;
	   value->gkey = dckey;
	   value->pitem = NULL;
	   int fd = dsi._fd; //open(dsi._filename.c_str(), O_RDONLY|O_DIRECT);
	   size_t bytes = 0;
	   while (bytes < dsi._size) {
	       size_t size = dsi._size;
	       if ((dsi._size&PAGESIZEMASK2) != 0) {
	           size = (dsi._size & PAGESIZEMASK1) + GCPAGESIZE;
	       }
	       size_t cur = pread(fd, value->data(), size, dsi._offset);
               if (cur == -1) { std::cout<<"read file error"<<std::endl; exit(0);}
	       bytes += cur;
	   }
	   //close(fd);
           return dynamic_cast<ValueTy*>(value);
        }
    }
}
template <class KeyTy, class ValueTy>
int GraphCached<KeyTy, ValueTy>::write(KeyTy key, ValueTy& value, int wbFlag /* default = 0*/) {

}
} // namespace graphcached

#endif
