#ifndef _CACHEMANAGER_H_
#define _CACHEMANAGER_H_

#include <limits>
#include "DiskComponent.h"
#include "Hashtable.h"
#include "CachePolicy.h"
#include "LruPolicy.h"

namespace graphcached {

template <class KeyTy, class ValueTy>
class CacheManager {
protected:
    uint32_t cacheLineSizePower; //
    uint64_t cacheSize; // in Bytes
    uint64_t maxPartitionSize; // in Bytes
    uint32_t cacheLineSize;
    uint64_t cacheLineSizeMask1;
    uint64_t cacheLineSizeMask2;
    uint64_t freeCacheSize;

    Hashtable<KeyTy>* ht; 
    CachePolicy<KeyTy>* cachepolicy;
    std::mutex cacheMutex;
public:
    CacheManager(uint32_t clsp, uint64_t cs, uint64_t mps):
    cacheLineSizePower(clsp), cacheSize(cs), maxPartitionSize(mps)
    {
        cacheLineSize = 1ull<<cacheLineSizePower;
	cacheLineSizeMask1 = (std::numeric_limits<uint64_t>::max())<<cacheLineSizePower;
	cacheLineSizeMask2 = cacheLineSize - 1;
	freeCacheSize = cacheSize;
	// LRU policy is the default policy
	cachepolicy = new LruPolicy<KeyTy>();
	ht = new Hashtable<KeyTy>();
    };
    
    DiskComponent<KeyTy>* cache(uint64_t size, KeyTy key);
    DiskComponent<KeyTy>* recache(DiskComponent<KeyTy>* dc);
    DiskComponent<KeyTy>* find(KeyTy key);
    void release(DiskComponent<KeyTy>*);  
    void dump() {
        ht->dump();
	cachepolicy->dump();
    }
    uint32_t getCacheLineSize() {return cacheLineSize;}
};

template <class KeyTy, class ValueTy>
void CacheManager<KeyTy, ValueTy>::release(DiskComponent<KeyTy>* dc) {
    cachepolicy->add(dc);
}

template <class KeyTy, class ValueTy>
DiskComponent<KeyTy>* CacheManager<KeyTy, ValueTy>::find(KeyTy key) {
    auto dc =  ht->find(key);
    if (dc == nullptr)
        return dc;
    // inc refcount and set state
    int rc = dc->refcount;
    int expected = 0;
    int desired = 1;
    if (rc == 0 && dc->refcount.compare_exchange_strong(expected, desired)) {
        dc->state = 1;
	cachepolicy->remove(dc);
    }
    else {
        dc->refcount++;
    }
    return dc;
}

template <class KeyTy, class ValueTy>
DiskComponent<KeyTy>* CacheManager<KeyTy, ValueTy>::cache(uint64_t size, KeyTy key) {
    // cache and recache are the only two interfaces which can insert item and delete item
    // currently, we proctect this with a lock
    std::lock_guard<std::mutex> cLock(cacheMutex);
    
    //std::cout<<"cacheLineSizePower:"<<cacheLineSizePower<<std::endl;
    //std::cout<<"cacheSize:"<<cacheSize<<std::endl;
    //std::cout<<"cacheLineSizeMask1:0x"<<std::hex<<cacheLineSizeMask1<<std::endl;
    //std::cout<<"cacheLineSizeMask2:0x"<<cacheLineSizeMask2<<std::dec<<std::endl;
    //std::cout<<"freeCacheSize:"<<freeCacheSize<<std::endl;
    //align size with cacheLineSize
    if (size & cacheLineSizeMask2 != 0) 
        size = size & cacheLineSizeMask1 + cacheLineSize;

    // is there enough free space?
    while (freeCacheSize < size) {
        auto evicted = cachepolicy->evict(size - freeCacheSize, ht);
	freeCacheSize += evicted;
    }
    // new diskcomponent, add it to hashtable
    auto dc = reinterpret_cast<DiskComponent<KeyTy>*>(new ValueTy());
    dc->addr = mmap(0, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (dc->addr == MAP_FAILED) {
        perror("MAP FAILED");
	exit(0);
    }
    freeCacheSize -= size;
    dc->size = size;
    dc->curSize = 0;
    dc->refcount++;
    ht->insert(key, dc);
    return dc;
}

template <class KeyTy, class ValueTy>
DiskComponent<KeyTy>* CacheManager<KeyTy, ValueTy>::recache(DiskComponent<KeyTy>* dc) {
    std::lock_guard<std::mutex> cLock(cacheMutex);
    
    uint64_t size = dc->size - dc->curSize;
    while (freeCacheSize < size) {
        auto evicted = cachepolicy->evict(size - freeCacheSize, ht);
	freeCacheSize += evicted;
    }
    dc->addr = mremap(dc->addr, dc->curSize, dc->size, MREMAP_MAYMOVE);
    if (dc->addr == MAP_FAILED) {
        perror("MAP FAILED");
	exit(0);
    }
    return dc;
}

} // namespace GraphCached

#endif
