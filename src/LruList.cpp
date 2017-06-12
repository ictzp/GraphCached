#include "LruList.h"

using namespace GraphCached;

int LruList::push(DiskComponent* dc) {
    std::lock_guard<std::mutex> llguard(lruMutex);
    lst.push_front(dc);
    return 1;
}

void LruList::pop() {
    std::lock_guard<std::mutex> llguard(lruMutex);
    lst.pop_front()
}
