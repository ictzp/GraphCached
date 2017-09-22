#ifndef _MRU_H_
#define _MRU_H_

#include <list>
#include <mutex>
#include <atomic>
#include "DiskComponent.h"

namespace graphcached{

template <class KeyTy>
class MruList {
protected:
    std::list<DiskComponent<KeyTy>*> lst; // inner lru list
    std::atomic<uint32_t> mremapcount;
    std::atomic<uint32_t> munmapcount;
public:
    MruList(): mremapcount(0), munmapcount(0) {}
    void push_back(DiskComponent<KeyTy>*);
    void push_front(DiskComponent<KeyTy>*);
    uint64_t evict(uint64_t, Hashtable<KeyTy>*);
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
void MruList<KeyTy>::dump() {
    std::cout<<"MruList:"<<std::endl;
    std::cout<<"================="<<std::endl;
    for (auto it = lst.begin(); it != lst.end(); ++it) {
        std::cout<<*it<<std::endl;
    }
    std::cout<<"================="<<std::endl;
}
template <class KeyTy>
uint64_t MruList<KeyTy>::evict(uint64_t size, Hashtable<KeyTy>* ht) {
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
typename std::list<DiskComponent<KeyTy>*>::iterator MruList<KeyTy>::erase(typename std::list<DiskComponent<KeyTy>*>::iterator it) {
    return lst.erase(it);
}

template <class KeyTy>
void MruList<KeyTy>::push_back(DiskComponent<KeyTy>* dc) {
    lst.push_back(dc);
    dc->lruPos = --lst.end();
}

template <class KeyTy>
void MruList<KeyTy>::push_front(DiskComponent<KeyTy>* dc) {
    lst.push_front(dc);
    dc->lruPos = lst.begin();
}

template <class KeyTy>
class MruPolicy: public CachePolicy<KeyTy> {
protected:
    MruList<KeyTy> ll;
public:
    MruPolicy(): CachePolicy<KeyTy>() {
    }
    void remove(DiskComponent<KeyTy>*);
    uint64_t evict(uint64_t, Hashtable<KeyTy>*);
    void add(DiskComponent<KeyTy>*);
    void dump() {ll.dump();}
    uint32_t getMremapcount() {return ll.getMremapcount();}
    uint32_t getMunmapcount() {return ll.getMunmapcount();}
};


template <class KeyTy>
void MruPolicy<KeyTy>::remove(DiskComponent<KeyTy>* dc) {
    ll.erase(dc->lruPos);
}

template <class KeyTy>
uint64_t MruPolicy<KeyTy>::evict(uint64_t size, Hashtable<KeyTy>* ht) {
    auto ret = ll.evict(size, ht);
    if (ret) {
    //std::cout<<"evit: ";
    //dump();
    }
    return ret;
}

template <class KeyTy>
void MruPolicy<KeyTy>::add(DiskComponent<KeyTy>* dc) {
    ll.push_front(dc);
    //std::cout<<"add: ";
    //dump();
}


} // namespace GraphCached

#endif
