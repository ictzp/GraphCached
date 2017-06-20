#ifndef _CACHEPOLICY_H_
#define _CACHEPOLICY_H_

namespace graphcached {

template <class KeyTy>
class CachePolicy {
public:
    virtual void remove(DiskComponent<KeyTy>*) = 0;
    virtual uint64_t evict(uint64_t, Hashtable<KeyTy>*) = 0;
    virtual void add(DiskComponent<KeyTy>*) = 0;
  
};

}

#endif