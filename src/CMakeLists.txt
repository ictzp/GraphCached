project(graphcached)
set(CORE_LIBRARY_NAME graphcached)
set(IO_LIBRARY_NAME "${LIBRARY_NAME}_io")

enable_language(C CXX)

set(CORE_SRC_FILES GraphCached.cpp)

## set(CMAKE_INSTALL_SO_NO_EXE 0)

message(STATUS "libgraphcached_source_dir: ${LibGraphCached_SOURCE_DIR}")

#link_directories(${CMAKE_BINARY_DIR}/3rd/memcached-1.4.36)
message(STATUS "CMAKE_BINARY_DIR: ${CMAKE_BINARY_DIR}")
add_library(${CORE_LIBRARY_NAME}
            SHARED
            ${CORE_SRC_FILES})

target_link_libraries(${CORE_LIBRARY_NAME} memcached)

install(TARGETS
        ${CORE_LIBRARY_NAME}
        LIBRARY 
        DESTINATION lib)
