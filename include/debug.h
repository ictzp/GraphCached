#ifndef _DEBUG_H_
#define _DEBUG_H_

#include <sys/time.h>

namespace graphcached {


#define DEBUG

#ifdef DEBUG
#define D(x) x
#else
#define D(x)
#endif

inline double getTime() {
    struct timeval tv;
    gettimeofday(&tv, NULL);\
    return tv.tv_sec + (tv.tv_usec / 1e6);
}

}

#endif
