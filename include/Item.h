#ifndef _ITEM_H_
#define _ITEM_H_

namespace GraphCached {

class Item {
public:
    // protected by LRU locks
    Item *next;
    Item *prev;
    // protected by an item lock
    Item *h_next; // hash chain next
    void *data;   // edge partition data
};

} // namespace GraphCached

#endif

