#ifndef _LRULIST_H_
#define _LRULIST_H_

#include <list>
#include <mutex>
#include "GraphCached.h"

namespace GraphCached{

class LruList {
protected:
    std::list<DiskComponent*> lst; // inner lru list
    std::mutex lruMutex;
public:
    int push(DiskComponent*);
    void pop();
};

} // namespace GraphCached

#endif
