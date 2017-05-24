project(test)
set(HIRT_LIB memcached)

#link_directories(${LibHIRT_SOURCE_DIR}/src)

macro(add_libmola_test name libraries)
  add_executable(${name}.run
                 ${name}.cpp)
  set(extra_args ${ARGN})

  target_link_libraries(${name}.run
                        ${libraries} ${extra_args})
endmacro(add_libmola_test)


add_libmola_test(test_GraphCached ${HIRT_LIB})

