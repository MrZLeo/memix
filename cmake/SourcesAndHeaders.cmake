set(sources
    ${CMAKE_CURRENT_SOURCE_DIR}/src/store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/view.cpp
)

file(GLOB bin_sources CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bin/*.cpp
)

set(headers
    ${CMAKE_CURRENT_SOURCE_DIR}/include/memix/store.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/memix/view.hpp
)

set(test_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/test/src/store_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/src/view_test.cpp
)
