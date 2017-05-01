set(LIBRARY_NAME graphcached)
set(IO_LIBRARY_NAME "${LIBRARY_NAME}_io")

enable_language(C CXX)

set(CORE_SRC_FILES GraphCached.cpp)

## set(CMAKE_INSTALL_SO_NO_EXE 0)


add_library(${CORE_LIBRARY_NAME}
            SHARED
            ${CORE_SRC_FILES})

#target_link_libraries(${CORE_LIBRARY_NAME})

install(TARGETS
        ${CORE_LIBRARY_NAME}
        LIBRARY 
        DESTINATION lib)
