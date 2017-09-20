#ifndef _DEBUG_H_
#define _DEBUG_H_

#include <mutex>
#include <sys/time.h>

namespace graphcached {


#define RECORD

#ifdef DEBUG
#define D(x) x
#else
#define D(x)
#endif

class BusyTimer {
private:
    int busy;
    std::mutex busyMtx;
    double startT;
    double sum;

public:
    BusyTimer() {
        busy = sum = startT = 0;
    }
    int set_busy() {
        std::lock_guard<std::mutex> lock(busyMtx);
        busy++;
        if (busy == 1) {
            startT = get_time();
            return 1;
        }
        return 0;
    }
    
    int unset_busy() {
        std::lock_guard<std::mutex> lock(busyMtx);
        busy--;
        if (busy == 0) {
            sum += get_time() - startT;
            return 1;
        }
        return 0;
    }

    double get_sum() {
        return sum;
    }

};

inline double getTime() {
    struct timeval tv;
    gettimeofday(&tv, NULL);\
    return tv.tv_sec + (tv.tv_usec / 1e6);
}

}

#endif
