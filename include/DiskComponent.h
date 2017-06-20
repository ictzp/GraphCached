#ifndef _DISKCOMPONENT_H_
#define _DISKCOMPONENT_H_

#include <atomic>
#include <list>

namespace graphcached {

#define PAGESIZEMASK1 0xfffffffffffff000ull
#define PAGESIZEMASK2 0xfffull
#define GCPAGESIZE 4096

void print_key(const char* key, size_t nkey) {
    int i;
    printf("0x");
    for (i = 0; i < nkey - 1; i++) {
        printf("%02x-", key[i]);
    }
    printf("%02x", key[i]);
}

class DiskSegmentInfo {
public:
	int _fd;
	off_t _offset;
	size_t _size;
public:
	DiskSegmentInfo(int fn, off_t off, size_t sz) : _fd(fn), _offset(off), _size(sz){}
	DiskSegmentInfo(): _fd(0), _offset(0), _size(0) {}
	size_t size() {return _size;}
};



template <class KeyTy>
class DiskComponent {
public:
	DiskSegmentInfo dsi;
	KeyTy gkey;
	uint64_t size; // size is aligned with cacheLineSize,
	               // which is the total size in memory
	uint64_t curSize;
	// refcount is zero if no threads hold this
	// else it indicates the number of theads holding it
        std::atomic<int> refcount;
	// state:
	// 1: refcout > 0 && data is ready (active)
	// 0: refcout = 0 && data is ready (inactive)
	// -1: partially evicted
	// -2: all data evicted or not initialized
	std::atomic<int> state;
        void* addr;
	typename std::list<DiskComponent<KeyTy>*>::iterator lruPos;
public:
	DiskComponent() {
	    refcount = 0;
	    state = -2;
	}
	DiskComponent(KeyTy key): gkey(key) 
	{
	    refcount = 0;
	    state = -2;
	}
	~DiskComponent() {}
	KeyTy& getKey() {return gkey;}
	// return aligned data address
};

} // namespace graphcached
#endif
