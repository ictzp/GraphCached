#ifndef _CACHEMANAGER_H_
#define _CACHEMANAGER_H_

namespace GraphCached {

class CacheManager() {
protected:
    size_t cacheLineSize; // in KB
    size_t cacheSize; // in KB
    size_t maxPartitionSize; // in KB
public:
    CacheManager(size_t cls, size_t cs, size_t mps):
    cacheLineSize(cls), cacheSize(cs), maxPartitionSize(mps)
    {};
    
};

} // namespace GraphCached

#endif
