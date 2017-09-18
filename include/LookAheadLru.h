#ifndef _LOOKAHEADLRU_H_
#define _LOOKAHEADLRU_H_

#include <list>
#include <mutex>
#include <atomic>
#include "DiskComponent.h"

namespace graphcached{

template <class KeyTy>
class LookAheadLruList {
protected:
    std::list<DiskComponent<KeyTy>*> lst; // inner lru list
    std::atomic<uint32_t> mremapcount;
    std::atomic<uint32_t> munmapcount;
public:
    LookAheadLruList(): mremapcount(0), munmapcount(0) {}
    void push_back(DiskComponent<KeyTy>*);
    void push_front(DiskComponent<KeyTy>*);
    uint64_t evict(uint64_t, Hashtable<KeyTy>*, typename std::list<DiskComponent<KeyTy>*>::iterator* );
    typename std::list<DiskComponent<KeyTy>*>::iterator erase(typename std::list<DiskComponent<KeyTy>*>::iterator);
    void dump();
    uint32_t getMremapcount() {return mremapcount;}
    uint32_t getMunmapcount() {return munmapcount;}
    int size() {
        return lst.size();
    }
    typename std::list<DiskComponent<KeyTy>*>::iterator
    begin() {
        return lst.begin();
    }
    typename std::list<DiskComponent<KeyTy>*>::iterator
    insert(typename std::list<DiskComponent<KeyTy>*>::iterator it, DiskComponent<KeyTy>* dc) {
        auto pos = lst.insert(it, dc);
        dc->lruPos = pos;
        return pos;
    }
    typename std::list<DiskComponent<KeyTy>*>::iterator
    end() {
        return lst.end();
    }
};

template <class KeyTy>
void LookAheadLruList<KeyTy>::dump() {
    std::cout<<"LookAheadList:"<<std::endl;
    std::cout<<"================="<<std::endl;
    for (auto it = lst.begin(); it != lst.end(); ++it) {
        std::cout<<*it<<std::endl;
    }
    std::cout<<"================="<<std::endl;
}
template <class KeyTy>
uint64_t LookAheadLruList<KeyTy>::evict(uint64_t size, Hashtable<KeyTy>* ht, typename std::list<DiskComponent<KeyTy>*>::iterator* watermark) {
    uint64_t remain = size;
    while (remain > 0) {
        auto dc = *(lst.begin());
	if (dc == nullptr) return size-remain;
	if (dc->curSize <= remain) {
	    // evict the whole dc
        if (*watermark == lst.begin())
            (*watermark)++;
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
            std::cout<<"args: "<<dc->addr<<" "<<dc->curSize<<" "<<dc->curSize-remain<<std::endl;
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
typename std::list<DiskComponent<KeyTy>*>::iterator LookAheadLruList<KeyTy>::erase(typename std::list<DiskComponent<KeyTy>*>::iterator it) {
    return lst.erase(it);
}

template <class KeyTy>
void LookAheadLruList<KeyTy>::push_back(DiskComponent<KeyTy>* dc) {
    lst.push_back(dc);
    dc->lruPos = --lst.end();
}

template <class KeyTy>
void LookAheadLruList<KeyTy>::push_front(DiskComponent<KeyTy>* dc) {
    lst.push_front(dc);
    dc->lruPos = lst.begin();
}

template <class KeyTy>
class LookAheadLruPolicy: public CachePolicy<KeyTy> {
protected:
    LookAheadLruList<KeyTy> ll;
    int activeP[512];
    typename std::list<DiskComponent<KeyTy>*>::iterator watermark;
public:
    LookAheadLruPolicy(): CachePolicy<KeyTy>() {
        for (int i = 0; i < 512; i++)
	        activeP[i] = 0;
        watermark = ll.end();
    }
    void remove(DiskComponent<KeyTy>*);
    uint64_t evict(uint64_t, Hashtable<KeyTy>*);
    void add(DiskComponent<KeyTy>*);
    void dump() {ll.dump();std::cout<<"watermark: "<<(*watermark)<<std::endl;}
    uint32_t getMremapcount() {return ll.getMremapcount();}
    uint32_t getMunmapcount() {return ll.getMunmapcount();}
    void reorder(size_t pid) {
        activeP[pid] = 1;
        auto sz = ll.size();
        if (sz == 0) return;
        for (auto it = ll.begin(); it != watermark;) {
            if (std::get<0>((*it)->gkey) == pid) {
                if (watermark == ll.end()) {
                    watermark = ll.insert(ll.end(), *it);
                }
                else {
                    ll.insert(ll.end(), *it);
                }
                it = ll.erase(it);
            }
            else {
                ++it;
            }
        }
     //   std::cout<<"reorder: ";
     //   dump();
    }
    void endIter() {
        for (int i = 0; i < 512; i++)
	    activeP[i] = 0;
    }
};


template <class KeyTy>
void LookAheadLruPolicy<KeyTy>::remove(DiskComponent<KeyTy>* dc) {
     if (watermark == dc->lruPos) {
         watermark = ll.erase(dc->lruPos);
     }
     else {
         ll.erase(dc->lruPos);
     }
     //std::cout<<"remove: ";
     //dump();
}

template <class KeyTy>
uint64_t LookAheadLruPolicy<KeyTy>::evict(uint64_t size, Hashtable<KeyTy>* ht) {
    auto ret = ll.evict(size, ht, &watermark);
    if (ret) {
    //std::cout<<"evit: ";
    //dump();
    }
    return ret;
}

template <class KeyTy>
void LookAheadLruPolicy<KeyTy>::add(DiskComponent<KeyTy>* dc) {
    if (activeP[std::get<0>(dc->gkey)] == 1) {
        if (watermark == ll.end()) {
            watermark = ll.insert(ll.end(), dc);
        }
        else {
            ll.insert(ll.end(), dc);
        }
    }
    else 
        ll.insert(watermark, dc);
    //std::cout<<"add: ";
    //dump();
}


} // namespace GraphCached

#endif
