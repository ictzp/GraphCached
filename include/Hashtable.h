#ifndef _HASHTABLE_H_
#define _HASHTABLE_H_

#include <unordered_map>
#include <mutex>
#include "GraphCached.h"
namespace GraphCached {

//#define hashsize(n) ((ub4)1<<(n))
//#define hashmask(n) (hashsize(n)-1)

//#define DEFAULT_HASHPOWER 16

template <class KeyTy>
class Hashtable{
protected:
    using hashtable_t = std::unordered_map<KeyTy, DiskComponent*>; 
    std::mutex htMutex; 
    hashtable_t ht;
public:
    Hashtable()
    ~Hashtable()
    int insert(KeyTy key, DiskComponent* dc);
    DiskComponent* erase(KeyTy key);
    DiskComponent* find(KeyTy key);
};

} // namespace GraphCached

#endif
