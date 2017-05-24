#include <new>
#include <fcntl.h>

#include "GraphCached.h"
#include "memcached.h"

using namespace graphcached;
const int UPDATE_LRU = 1;

template <class KeyTy, class ValueTy>
ValueTy* GraphCached<KeyTy, ValueTy>::read(KeyTy key, int cacheFlag /* default = 1*/) {
    char* dckey = static_cast<char*>(&key);
    size_t ndckey = sizeof(key);
    item* it;
    it = item_get(dckey, ndckey, NULL, UPDATE_LRU);
    if (it != NULL) {
        return static_cast<ValueTy*>(ITEM_data(it));  
    }
    else {
       if (cacheFlag) {
           DiskSegmentInfo dsi = plocate(key);
	   it = item_alloc(dckey, ndckey, 0, 0/*expiration time, 0 means never expire*/, dsi.size()+ sizeof(ValueTy));
	   if (it != NULL) {
	       // use placement new to place the new object at the allocated address
	       DiskComponent* value = new(static_cast<void*>(ITEM_data(it))) ValueTy();
	       value->dsi = dsi;
	       value->gkey = dckey;
	       int fd = open(dsi._filename.c_str(), O_RDONLY);
	       size_t bytes = 0;
	       while (bytes < dsi._size) {
	           size_t cur = pread(fd, value->data, dsi._size - bytes, dsi._offset + bytes);
		   bytes += cur;
	       }
	       close(fd);
	       return value;
	   }
	   else {
	       fprintf(stderr, "allocation error!\n");
	       return NULL;
	   }
       }
       else {
           DiskSegmentInfo dsi = plocate(key);
	   DiskComponent * value = new ValueTy();
	   value->dsi = dsi;
	   value->gkey = dckey;
	   int fd = open(dsi._filename.c_str(), O_RDONLY);
	   size_t bytes = 0;
	   while (bytes < dsi._size) {
	       size_t cur = pread(fd, value->data, dsi._size - bytes, dsi._offset + bytes);
	       bytes += cur;
	   }
	   close(fd);
           return value;
       }
    }
}
template <class KeyTy, class ValueTy>
int GraphCached<KeyTy, ValueTy>::write(KeyTy key, ValueTy& value, int wbFlag /* default = 0*/) {

}

//int main(int argc, char**argv) {
//GraphCached<int, std::string> graph("a");
//graph.read(0, 0);
//
//return 0;
//}
