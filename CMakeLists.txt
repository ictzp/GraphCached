project(LibGraphCached)

cmake_minimum_required(VERSION 2.8)

include_directories(${LibGraphCached_SOURCE_DIR}/include)

## enable the C++11 features
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -pthread -D__FILENAME__='\"$(subst ${CMAKE_SOURCE_DIR}/,,$(abspath $<))\"'")

option(BUILD_TESTS "Enable to build the tests programs" off)

option(DEBUG_INFO "Enable to show the console debug info" off)

if(${DEBUG_INFO})
  add_definitions(-DENABLE_DEBUG_INFO)
endif()

## check the arch of current building system
execute_process( COMMAND uname -m COMMAND tr -d '\n' OUTPUT_VARIABLE PLAT_ARCH)

message(STATUS "Platfrom architecture : ${PLAT_ARCH}")

##if(NOT ${PLAT_ARCH} MATCHES "x86_64")
##  add_definitions(-DUSE_UCONTEXT=1)
##endif()

#option(USE_UCONTEXT_API "Enable ucontext based fiber mechnism" off)
#if(${USE_UCONTEXT_API})
#  add_definitions(-DUSE_UCONTEXT=1)
#endif()

add_subdirectory(src)

install(DIRECTORY 
        "include"
        DESTINATION 
        "include"
        FILES_MATCHING
        PATTERN
        "*.h")

if(BUILD_TESTS)
  add_subdirectory(tests)
endif(BUILD_TESTS)

