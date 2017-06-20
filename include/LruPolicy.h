#ifndef _LRULIST_H_
#define _LRULIST_H_

#include <list>
#include <mutex>
#include "DiskComponent.h"

namespace graphcached{

template <class KeyTy>
class LruList {
protected:
    std::list<DiskComponent<KeyTy>*> lst; // inner lru list
    std::mutex lruMutex;
public:
    void push(DiskComponent<KeyTy>*);
    uint64_t evict(uint64_t, Hashtable<KeyTy>*);
    void erase(typename std::list<DiskComponent<KeyTy>*>::iterator);
};

template <class KeyTy>
class LruPolicy: public CachePolicy<KeyTy> {
protected:
    LruList<KeyTy> ll;
public:
    void remove(DiskComponent<KeyTy>*);
    uint64_t evict(uint64_t, Hashtable<KeyTy>*);
    void add(DiskComponent<KeyTy>*);
};

template <class KeyTy>
uint64_t LruList<KeyTy>::evict(uint64_t size, Hashtable<KeyTy>* ht) {
    std::lock_guard<std::mutex> llLock(lruMutex);
    uint64_t remain = size;
    while (remain > 0) {
        auto dc = *(lst.begin());
	if (dc->curSize <= remain) {
	    // evict the whole dc
	    lst.pop_front();
	    ht->erase(dc);
	    auto ret = munmap(dc->addr, dc->curSize);
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
    return size - remain;
}

template <class KeyTy>
void LruList<KeyTy>::erase(typename std::list<DiskComponent<KeyTy>*>::iterator it) {
    std::lock_guard<std::mutex> llLock(lruMutex);
    lst.erase(it);
}

template <class KeyTy>
void LruList<KeyTy>::push(DiskComponent<KeyTy>* dc) {
    std::lock_guard<std::mutex> llLock(lruMutex);
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
