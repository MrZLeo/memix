set(sources
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tmp.cpp
)

file(GLOB bin_sources CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bin/*.cpp
)

set(headers
    ${CMAKE_CURRENT_SOURCE_DIR}/include/memix/tmp.hpp
)

set(test_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/test/src/tmp_test.cpp
)
