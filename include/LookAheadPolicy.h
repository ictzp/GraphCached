#ifndef _LOOKAHEAD_H_
#define _LOOKAHEAD_H_

#include <list>
#include <mutex>
#include <atomic>
#include "DiskComponent.h"

namespace graphcached{

template <class KeyTy>
class LookAheadList {
protected:
    std::list<DiskComponent<KeyTy>*> lst; // inner lru list
    std::mutex lruMutex;
    std::atomic<uint32_t> mremapcount;
    std::atomic<uint32_t> munmapcount;
public:
    LookAheadList(): mremapcount(0), munmapcount(0) {}
    void push_back(DiskComponent<KeyTy>*);
    void push_front(DiskComponent<KeyTy>*);
    uint64_t evict(uint64_t, Hashtable<KeyTy>*);
    void erase(typename std::list<DiskComponent<KeyTy>*>::iterator);
    void dump();
    uint32_t getMremapcount() {return mremapcount;}
    uint32_t getMunmapcount() {return munmapcount;}
    void reorder(size_t pid) {
        if (lst.empty()) return;
        auto sz = lst.size();
	int i = 0;
	for (auto it = lst.begin(); i < sz - 1; it++) {
	    i++;
	    if (std::get<0>((*it)->gkey) == pid) {
	        lst.push_back(*it);
		(*it)->lruPos = --lst.end();
		it = lst.erase(it); 
	    }
	}
    }
};

template <class KeyTy>
void LookAheadList<KeyTy>::dump() {
    std::cout<<"LookAheadList:"<<std::endl;
    std::cout<<"================="<<std::endl;
    for (auto it = lst.begin(); it != lst.end(); ++it) {
        std::cout<<*it<<std::endl;
    }
    std::cout<<"================="<<std::endl;
}
template <class KeyTy>
uint64_t LookAheadList<KeyTy>::evict(uint64_t size, Hashtable<KeyTy>* ht) {
    //std::lock_guard<std::mutex> llLock(lruMutex);
    uint64_t remain = size;
    while (remain > 0) {
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
void LookAheadList<KeyTy>::erase(typename std::list<DiskComponent<KeyTy>*>::iterator it) {
    //std::lock_guard<std::mutex> llLock(lruMutex);
    lst.erase(it);
}

template <class KeyTy>
void LookAheadList<KeyTy>::push_back(DiskComponent<KeyTy>* dc) {
    //std::lock_guard<std::mutex> llLock(lruMutex);
    lst.push_back(dc);
    dc->lruPos = --lst.end();
}

template <class KeyTy>
void LookAheadList<KeyTy>::push_front(DiskComponent<KeyTy>* dc) {
    //std::lock_guard<std::mutex> llLock(lruMutex);
    lst.push_front(dc);
    dc->lruPos = lst.begin();
}

template <class KeyTy>
class LookAheadPolicy: public CachePolicy<KeyTy> {
protected:
    LookAheadList<KeyTy> ll;
    int activeP[512];
public:
    LookAheadPolicy(): CachePolicy<KeyTy>() {
        for (int i = 0; i < 512; i++)
	    activeP[i] = 0;
    }
    void remove(DiskComponent<KeyTy>*);
    uint64_t evict(uint64_t, Hashtable<KeyTy>*);
    void add(DiskComponent<KeyTy>*);
    void dump() {ll.dump();}
    uint32_t getMremapcount() {return ll.getMremapcount();}
    uint32_t getMunmapcount() {return ll.getMunmapcount();}
    void reorder(size_t pid) {
        ll.reorder(pid);
        activeP[pid] = 1;
    }
    void endIter() {
        for (int i = 0; i < 512; i++)
	    activeP[i] = 0;
    }
};


template <class KeyTy>
void LookAheadPolicy<KeyTy>::remove(DiskComponent<KeyTy>* dc) {
     ll.erase(dc->lruPos);
}

template <class KeyTy>
uint64_t LookAheadPolicy<KeyTy>::evict(uint64_t size, Hashtable<KeyTy>* ht) {
    return ll.evict(size, ht);
}

template <class KeyTy>
void LookAheadPolicy<KeyTy>::add(DiskComponent<KeyTy>* dc) {
    if (activeP[std::get<0>(dc->gkey)] == 1)
        ll.push_back(dc);
    else 
        ll.push_front(dc);
}


} // namespace GraphCached

#endif
