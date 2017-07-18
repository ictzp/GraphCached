#ifndef _HASHTABLE_H_
#define _HASHTABLE_H_

#include <unordered_map>
#include <mutex>
#include "DiskComponent.h"
namespace graphcached {

//#define hashsize(n) ((ub4)1<<(n))
//#define hashmask(n) (hashsize(n)-1)

//#define DEFAULT_HASHPOWER 16

template <class KeyTy>
class Hashtable{
protected:
    using hashtable_t = std::unordered_map<KeyTy, DiskComponent<KeyTy>*>; 
    std::mutex htMutex; 
    hashtable_t ht;
public:
    Hashtable(){}
    ~Hashtable(){}
    int insert(KeyTy key, DiskComponent<KeyTy>* dc);
    void erase(DiskComponent<KeyTy>*);
    DiskComponent<KeyTy>* find(KeyTy key);
    void dump();
};

// print all the data in hash table
template <class KeyTy>
void Hashtable<KeyTy>::dump() {
    std::cout<<"Hashtable:"<<std::endl;
    std::cout<<"======================"<<std::endl;
    for (auto it = ht.begin(); it != ht.end(); ++it) {
        std::cout<<it->first<<" : "<<it->second<<std::endl;
        it->second->dump();
    }
    std::cout<<"======================"<<std::endl;
}

template <class KeyTy>
int Hashtable<KeyTy>::insert(KeyTy key, DiskComponent<KeyTy>* dc) {
    //std::lock_guard<std::mutex> htLock(htMutex);
    ht.insert(std::make_pair(key, dc));
}

template <class KeyTy>
void Hashtable<KeyTy>::erase(DiskComponent<KeyTy>* dc) {
    //std::lock_guard<std::mutex> htLock(htMutex);
    auto it = ht.erase(dc->gkey);
    if (it != 1) {
        std::cerr<<"Hashtable erase should have been 1."<<std::endl;
    }
}

template <class KeyTy>
DiskComponent<KeyTy>* Hashtable<KeyTy>::find(KeyTy key) {
//    std::lock_guard<std::mutex> htLock(htMutex);
    auto it = ht.find(key);
    if (it != ht.end())
        return it->second;
    else
        return nullptr;
}

} // namespace GraphCached

#endif
