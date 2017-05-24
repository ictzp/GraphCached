project(memcached)
set(MEMCACHED_LIBRARY_NAME "memcached")

enable_language(C CXX)

set(MEMCACHED_CORE_SRC_FILES memcached.c
                   itoa_ljust.c
#		   sasl_defs.c
		   util.c
		   bipbuffer.c
		   crawler.c
		   hash.c
#		   solaris_priv.c
		   cache.c
		   daemon.c
		   items.c
		   jenkins_hash.c
		   assoc.c
		   murmur3_hash.c
		   sizes.c
		   slabs.c
		   stats.c
		   thread.c
#		   timedrun.c
		   logger.c)

## set(CMAKE_INSTALL_SO_NO_EXE 0)


add_library(${MEMCACHED_LIBRARY_NAME}
            SHARED
            ${MEMCACHED_CORE_SRC_FILES})

#target_link_libraries(${CORE_LIBRARY_NAME})

install(TARGETS
        ${MEMCACHED_LIBRARY_NAME}
        LIBRARY 
        DESTINATION lib
	)
