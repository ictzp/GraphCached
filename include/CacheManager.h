#ifndef _CACHEMANAGER_H_
#define _CACHEMANAGER_H_

#include <limits>
#include <atomic>
#include "DiskComponent.h"
#include "Hashtable.h"
#include "CachePolicy.h"
#include "LruPolicy.h"
#include "LookAheadPolicy.h"
#include "MruPolicy.h"
#include "LookAheadLru.h"

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

    std::atomic<uint32_t> mmapcount;
    Hashtable<KeyTy>* ht; 
    CachePolicy<KeyTy>* cachepolicy;
    int policy;
    std::mutex cmMutex;
public:
    CacheManager(uint32_t clsp, uint64_t cs, uint64_t mps):
    cacheLineSizePower(clsp), cacheSize(cs), maxPartitionSize(mps)
    {
        cacheLineSize = 1ull<<cacheLineSizePower;
	cacheLineSizeMask1 = (std::numeric_limits<uint64_t>::max())<<cacheLineSizePower;
	cacheLineSizeMask2 = cacheLineSize - 1;
	freeCacheSize = cacheSize;
	// LRU policy is the default policy
	//cachepolicy = new LruPolicy<KeyTy>();
	//policy = 0;
	if (cacheSize % cacheLineSize) std::cout<<"incorrect cacheSize"<<std::endl;
	// LookAhead policy
        cachepolicy = new LookAheadPolicy<KeyTy>();
	policy = 1;
    //MRU policy
    //cachepolicy = new MruPolicy<KeyTy>();
    //policy = 2;
	//LookAheadLru policy
    //cachepolicy = new LookAheadLruPolicy<KeyTy>();
    //policy  = 3;

    ht = new Hashtable<KeyTy>();
	mmapcount = 0;
	//Block::blocksize = cacheLineSize;
    };
    
    
    DiskComponent<KeyTy>* cache(uint64_t size, KeyTy key, DiskComponent<KeyTy>* dc);
    DiskComponent<KeyTy>* recache(DiskComponent<KeyTy>* dc);
    DiskComponent<KeyTy>* find(KeyTy key);
    void release(DiskComponent<KeyTy>*);  
    void dump() {
        ht->dump();
	cachepolicy->dump();
    }
    void reorder(size_t pid) {
    std::lock_guard<std::mutex> llLock(cmMutex);
        if (policy == 1 || policy == 3)
            dynamic_cast<LookAheadPolicy<KeyTy>*>(cachepolicy)->reorder(pid);
    }
    void endIter() {
    std::lock_guard<std::mutex> llLock(cmMutex);
        if (policy == 1 || policy == 3)
	    dynamic_cast<LookAheadPolicy<KeyTy>*>(cachepolicy)->endIter();
    }
    uint64_t getCacheSize() {return cacheSize;}
    uint32_t getCacheLineSize() {return cacheLineSize;}
    uint32_t getMmapcount() {return mmapcount;}
    uint32_t getMremapcount() {return cachepolicy->getMremapcount();}
    uint32_t getMunmapcount() {return cachepolicy->getMunmapcount();}
};

template <class KeyTy, class ValueTy>
void CacheManager<KeyTy, ValueTy>::release(DiskComponent<KeyTy>* dc) {
    std::lock_guard<std::mutex> llLock(cmMutex);
    cachepolicy->add(dc);
}

template <class KeyTy, class ValueTy>
DiskComponent<KeyTy>* CacheManager<KeyTy, ValueTy>::find(KeyTy key) {
    std::lock_guard<std::mutex> llLock(cmMutex);
    auto dc =  ht->find(key);
    if (dc == nullptr)
        return dc;
    // inc refcount and set state
    //int rc = dc->refcount;
    //int expected = 0;
    //int desired = 1;
    //if (rc == 0 && dc->refcount.compare_exchange_strong(expected, desired)) {
    //    dc->state = 1;
    //    cachepolicy->remove(dc);
    //}
    //else {
    //    dc->refcount++;
    //
    dc->refcount++;
    if (dc->refcount == 1) {
	cachepolicy->remove(dc);
    }
    return dc;
}

template <class KeyTy, class ValueTy>
DiskComponent<KeyTy>* CacheManager<KeyTy, ValueTy>::cache(uint64_t sz, KeyTy key, DiskComponent<KeyTy>* dc) {
    std::lock_guard<std::mutex> llLock(cmMutex);
    // cache and recache are the only two interfaces which can insert item and delete item
    // currently, we proctect this with a lock
    //std::lock_guard<std::mutex> cLock(cacheMutex);
    
    //std::cout<<"cacheLineSizePower:"<<cacheLineSizePower<<std::endl;
    //std::cout<<"cacheSize:"<<cacheSize<<std::endl;
    //std::cout<<"cacheLineSizeMask1:0x"<<std::hex<<cacheLineSizeMask1<<std::endl;
    //std::cout<<"cacheLineSizeMask2:0x"<<cacheLineSizeMask2<<std::dec<<std::endl;
    //std::cout<<"freeCacheSize:"<<freeCacheSize<<std::endl;
    //align size with cacheLineSize
    auto size = sz;
    if ((sz & cacheLineSizeMask2) != 0) 
        size = (sz & cacheLineSizeMask1) + cacheLineSize;

    // is there enough free space?
    if (freeCacheSize < size) {
        auto evicted = cachepolicy->evict(size - freeCacheSize, ht);
	freeCacheSize += evicted;
    }
    if (freeCacheSize < size) return nullptr;
    // new diskcomponent, add it to hashtable
    dc->addr = mmap(0, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    mmapcount++;
    if (dc->addr == MAP_FAILED) {
        perror("MAP FAILED");
	exit(0);
    }
    freeCacheSize -= size;
    dc->size = size;
    dc->curSize = dc->size;
    dc->refcount++;
    ht->insert(key, dc);
    return dc;
}

template <class KeyTy, class ValueTy>
DiskComponent<KeyTy>* CacheManager<KeyTy, ValueTy>::recache(DiskComponent<KeyTy>* dc) {
    std::lock_guard<std::mutex> llLock(cmMutex);
    //std::lock_guard<std::mutex> cLock(cacheMutex);
    
    uint64_t size = dc->size - dc->curSize;
    if (freeCacheSize < size) {
        auto evicted = cachepolicy->evict(size - freeCacheSize, ht);
	freeCacheSize += evicted;
    }
    if (freeCacheSize < size) return nullptr;
    dc->addr = mremap(dc->addr, dc->curSize, dc->size, MREMAP_MAYMOVE);
    if (dc->addr == MAP_FAILED) {
        perror("MAP FAILED");
	exit(0);
    }
    freeCacheSize -= size;
    return dc;
}

} // namespace GraphCached

#endif
