#include <iostream>
#include "Hashtable.h"

using namespace GraphCached;

template <class KeyTy>
Hashtable<KeyTy>::Hashtable() {
}

template <class KeyTy>
Hashtable<KeyTy>::~Hashtable() {
}

template <class KeyTy>
int Hashtable<KeyTy>::insert(KeyTy key, DiskComponent* dc) {
   std::lock_guard<std::mutex> htguard(htMutex);
   ht[key] = dc;
   return 1;
}

template <class KeyTy>
DiskComponent* erase(KeyTy key) {
    std::lock_guard<std::mutex> htguard(htMutex);
    auto it = ht.find(key);
    ht.erase(it);
    return it->second;
}

template <class KeyTy>
DiskComponent* find(KeyTy key) {
    auto it =  ht.find(key);
    if (it == hashtable_t::end)
        return nullptr;
    else
        return it->second;
}


