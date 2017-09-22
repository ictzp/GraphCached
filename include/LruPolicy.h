#ifndef _LRULIST_H_
#define _LRULIST_H_

#include <list>
#include <mutex>
#include <atomic>
#include "DiskComponent.h"

namespace graphcached{

template <class KeyTy>
class LruList {
protected:
    std::list<DiskComponent<KeyTy>*> lst; // inner lru list
    std::mutex lruMutex;
    std::atomic<uint32_t> mremapcount;
    std::atomic<uint32_t> munmapcount;
public:
    LruList(): mremapcount(0), munmapcount(0) {}
    void push(DiskComponent<KeyTy>*);
    uint64_t evict(uint64_t, Hashtable<KeyTy>*);
    void erase(typename std::list<DiskComponent<KeyTy>*>::iterator);
    void dump();
    uint32_t getMremapcount() {return mremapcount;}
    uint32_t getMunmapcount() {return munmapcount;}
};

template <class KeyTy>
void LruList<KeyTy>::dump() {
    std::cout<<"LruList:"<<std::endl;
    std::cout<<"================="<<std::endl;
    for (auto it = lst.begin(); it != lst.end(); ++it) {
        std::cout<<*it<<std::endl;
    }
    std::cout<<"================="<<std::endl;
}

template <class KeyTy>
class LruPolicy: public CachePolicy<KeyTy> {
protected:
    LruList<KeyTy> ll;
public:
    void remove(DiskComponent<KeyTy>*);
    uint64_t evict(uint64_t, Hashtable<KeyTy>*);
    void add(DiskComponent<KeyTy>*);
    void dump() {ll.dump();}
    uint32_t getMremapcount() {return ll.getMremapcount();}
    uint32_t getMunmapcount() {return ll.getMunmapcount();}
};

template <class KeyTy>
uint64_t LruList<KeyTy>::evict(uint64_t size, Hashtable<KeyTy>* ht) {
    //std::lock_guard<std::mutex> llLock(lruMutex);
    uint64_t remain = size;
    while (remain > 0) {
    if (lst.empty()) return size-remain;
    auto dc = *(lst.begin());
	if (dc->curSize <= remain) {
	    // evict the whole dc
	    lst.pop_front();
	    ht->erase(dc);
	    auto ret = munmap(dc->addr, dc->curSize);
	    munmapcount++;
	    if (ret == -1) {
	        perror("munmap failed");
		exit(0);
	    }
	    remain -= dc->curSize;
	    delete dc;
	}
	else {
	    // evit part of the dc
            dc->addr = mremap(dc->addr, dc->curSize, dc->curSize-remain, 0);
	    mremapcount++;
	    if (dc->addr == MAP_FAILED) {
	        perror("mremap failed");
		exit(0);
	    }
	    dc->curSize -= remain;
	    remain = 0;
	    // change state
	    dc->state = -1; 
	}
    }
    return size;
}

template <class KeyTy>
void LruList<KeyTy>::erase(typename std::list<DiskComponent<KeyTy>*>::iterator it) {
    //std::lock_guard<std::mutex> llLock(lruMutex);
    lst.erase(it);
}

template <class KeyTy>
void LruList<KeyTy>::push(DiskComponent<KeyTy>* dc) {
    //std::lock_guard<std::mutex> llLock(lruMutex);
    lst.push_back(dc);
    dc->lruPos = --lst.end();
}

template <class KeyTy>
void LruPolicy<KeyTy>::remove(DiskComponent<KeyTy>* dc) {
    ll.erase(dc->lruPos);
}

template <class KeyTy>
uint64_t LruPolicy<KeyTy>::evict(uint64_t size, Hashtable<KeyTy>* ht) {
    return ll.evict(size, ht);
}

template <class KeyTy>
void LruPolicy<KeyTy>::add(DiskComponent<KeyTy>* dc) {
    ll.push(dc);
}


} // namespace GraphCached

#endif
