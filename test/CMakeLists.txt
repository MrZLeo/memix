CPMAddPackage(
  NAME Catch2
  GITHUB_REPOSITORY catchorg/Catch2
  VERSION 3.11.0
  SYSTEM YES
  OPTIONS
    "BUILD_SHARED_LIBS OFF"
    "BUILD_TESTING OFF"
    "CATCH_INSTALL_DOCS OFF"
    "CATCH_INSTALL_EXTRAS ON"
)

# CPM's find_package scope does not preserve an installed package's module path.
if(Catch2_SOURCE_DIR)
  include("${Catch2_SOURCE_DIR}/extras/Catch.cmake")
elseif(catch2_SOURCE_DIR)
  include("${catch2_SOURCE_DIR}/extras/Catch.cmake")
else()
  include("${Catch2_DIR}/Catch.cmake")
endif()
find_package(Threads REQUIRED)

if(memix_ENABLE_CODE_COVERAGE)
  add_compile_options(-O0 -g --coverage)
endif()

foreach(file IN LISTS test_sources)
  get_filename_component(test_name "${file}" NAME_WE)
  set(test_cli)
  if(test_name STREQUAL "quest_test" OR test_name STREQUAL "store_test" OR test_name STREQUAL "cli_test")
    set(test_cli memix_memix)
  endif()
  if(test_cli AND NOT TARGET "${test_cli}")
    message(STATUS "Skipping ${test_name}: requires CLI target ${test_cli}.")
    continue()
  endif()

  add_executable(${test_name}_Tests "${file}")
  set_project_warnings(${test_name}_Tests)
  target_link_libraries(${test_name}_Tests PRIVATE Catch2::Catch2WithMain memix::memix Threads::Threads)
  target_include_directories(${test_name}_Tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
  if(test_name STREQUAL "json_test")
    target_link_libraries(${test_name}_Tests PRIVATE memix_simdjson_headers)
  endif()
  if(memix_STATIC_LINK AND (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR MINGW))
    target_link_options(${test_name}_Tests PRIVATE -static)
  endif()
  if(test_cli)
    add_dependencies(${test_name}_Tests ${test_cli})
    target_compile_definitions(${test_name}_Tests PRIVATE
      MEMIX_TEST_BINARY="$<TARGET_FILE:${test_cli}>"
    )
    if(test_name STREQUAL "cli_test")
      add_dependencies(${test_name}_Tests memix_memix-store memix_memix-view memix_memix-tui)
      target_compile_definitions(${test_name}_Tests PRIVATE
        MEMIX_STORE_TEST_BINARY="$<TARGET_FILE:memix_memix-store>"
        MEMIX_VIEW_TEST_BINARY="$<TARGET_FILE:memix_memix-view>"
        MEMIX_TUI_TEST_BINARY="$<TARGET_FILE:memix_memix-tui>"
      )
    endif()
  endif()
  catch_discover_tests(${test_name}_Tests)
endforeach()
