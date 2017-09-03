project(LibGraphCached)

cmake_minimum_required(VERSION 2.8)

include_directories(${LibGraphCached_SOURCE_DIR}/include)
include_directories(${LibGraphCached_SOURCE_DIR}/3rd/memcached-1.4.36)

message(STATUS "source dir : ${LibGraphCached_SOURCE_DIR}")

## enable the C++11 features
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -pthread -D__FILENAME__='\"$(subst ${CMAKE_SOURCE_DIR}/,,$(abspath $<))\"'")

## memcached is written in C99
##set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -std=c99 -pthread")

option(BUILD_TESTS "Enable to build the tests programs" off)

option(DEBUG_INFO "Enable to show the console debug info" off)

if(${DEBUG_INFO})
  add_definitions(-DENABLE_DEBUG_INFO)
endif()

## check the arch of current building system
execute_process( COMMAND uname -m COMMAND tr -d '\n' OUTPUT_VARIABLE PLAT_ARCH)

message(STATUS "Platfrom architecture : ${PLAT_ARCH}")

include(CPack)
include(TestBigEndian)
TEST_BIG_ENDIAN(IS_BIG_ENDIAN)
if(NOT ${IS_BIG_ENDIAN})
  add_definitions(-D__LITTLE_ENDIAN__)
endif()

##if(NOT ${PLAT_ARCH} MATCHES "x86_64")
##  add_definitions(-DUSE_UCONTEXT=1)
##endif()

#option(USE_UCONTEXT_API "Enable ucontext based fiber mechnism" off)
#if(${USE_UCONTEXT_API})
#  add_definitions(-DUSE_UCONTEXT=1)
#endif()
#add_subdirectory(src)
#add_subdirectory(3rd/memcached-1.4.36)

install(FILES
        "include/GraphCached.h"             "include/Hashtable.h"            "include/DiskComponent.h"
	"include/CacheManager.h"            "include/CachePolicy.h"          "include/LruPolicy.h"
        "include/queue.hpp"                 "include/LookAheadPolicy.h"      "include/debug.h"
#	"3rd/memcached-1.4.36/memcached.h"
#        "3rd/memcached-1.4.36/assoc.h"      "3rd/memcached-1.4.36/itoa_ljust.h"     "3rd/memcached-1.4.36/protocol_binary.h"
#	"3rd/memcached-1.4.36/bipbuffer.h"  "3rd/memcached-1.4.36/jenkins_hash.h"   "3rd/memcached-1.4.36/sasl_defs.h"
#	"3rd/memcached-1.4.36/cache.h"      "3rd/memcached-1.4.36/logger.h"         "3rd/memcached-1.4.36/slabs.h"
#	"3rd/memcached-1.4.36/crawler.h"    "3rd/memcached-1.4.36/memcached.h"      "3rd/memcached-1.4.36/stats.h"
#	"3rd/memcached-1.4.36/hash.h"       "3rd/memcached-1.4.36/murmur3_hash.h"   "3rd/memcached-1.4.36/trace.h"
#	"3rd/memcached-1.4.36/items.h"      "3rd/memcached-1.4.36/util.h"
	DESTINATION 
        "include"
        )

if(BUILD_TESTS)
  add_subdirectory(tests)
endif(BUILD_TESTS)

