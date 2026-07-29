cmake_minimum_required(VERSION 3.25)

#
# Project details
#

project(
  ${CMAKE_PROJECT_NAME}Tests
  LANGUAGES CXX
)

verbose_message("Adding tests under ${CMAKE_PROJECT_NAME}Tests...")

CPMAddPackage(
  NAME Catch2
  GITHUB_REPOSITORY catchorg/Catch2
  VERSION 3.11.0
  OPTIONS
    "BUILD_TESTING OFF"
    "CATCH_INSTALL_DOCS OFF"
    "CATCH_INSTALL_EXTRAS ON"
)

if(DEFINED Catch2_SOURCE_DIR)
  set(CATCH2_EXTRAS_DIR "${Catch2_SOURCE_DIR}/extras")
elseif(DEFINED catch2_SOURCE_DIR)
  set(CATCH2_EXTRAS_DIR "${catch2_SOURCE_DIR}/extras")
else()
  message(FATAL_ERROR "Catch2 extras directory could not be determined.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CATCH2_EXTRAS_DIR}")
include(Catch)

find_package(Threads REQUIRED)

foreach(file ${test_sources})
  string(REGEX REPLACE "(.*/)([a-zA-Z0-9_ ]+)(\.cpp)" "\\2" test_name ${file})
  add_executable(${test_name}_Tests ${file})

  project_apply_cxx_standard(${test_name}_Tests PRIVATE)

  set(${CMAKE_PROJECT_NAME}_TEST_LIB ${CMAKE_PROJECT_NAME})

  #
  # Setup code coverage if enabled
  #

  if (${CMAKE_PROJECT_NAME}_ENABLE_CODE_COVERAGE)
    get_target_property(_project_test_lib_type ${${CMAKE_PROJECT_NAME}_TEST_LIB} TYPE)
    if(_project_test_lib_type STREQUAL "INTERFACE_LIBRARY")
      set(_project_test_lib_visibility INTERFACE)
    else()
      set(_project_test_lib_visibility PUBLIC)
    endif()

    target_compile_options(
      ${${CMAKE_PROJECT_NAME}_TEST_LIB}
      ${_project_test_lib_visibility}
      -O0
      -g
      -fprofile-arcs
      -ftest-coverage
    )
    target_link_options(
      ${${CMAKE_PROJECT_NAME}_TEST_LIB}
      ${_project_test_lib_visibility}
      -fprofile-arcs
      -ftest-coverage
    )
    verbose_message("Code coverage is enabled and provided with GCC.")
  endif()

  target_link_libraries(
    ${test_name}_Tests
    PRIVATE
      Catch2::Catch2WithMain
      ${${CMAKE_PROJECT_NAME}_TEST_LIB}
      Threads::Threads
  )
  target_include_directories(
    ${test_name}_Tests
    PRIVATE
      ${CMAKE_SOURCE_DIR}/src
  )

  if(test_name STREQUAL "quest_test")
    add_dependencies(${test_name}_Tests ${CMAKE_PROJECT_NAME}_memix-view)
    target_compile_definitions(
      ${test_name}_Tests
      PRIVATE
        MEMIX_VIEW_TEST_BINARY="$<TARGET_FILE:${CMAKE_PROJECT_NAME}_memix-view>"
    )
  endif()

  if(test_name STREQUAL "store_test")
    add_dependencies(${test_name}_Tests ${CMAKE_PROJECT_NAME}_memix-store)
    target_compile_definitions(
      ${test_name}_Tests
      PRIVATE
        MEMIX_STORE_TEST_BINARY="$<TARGET_FILE:${CMAKE_PROJECT_NAME}_memix-store>"
    )
  endif()

  catch_discover_tests(${test_name}_Tests)
endforeach()

verbose_message("Finished adding unit tests for ${CMAKE_PROJECT_NAME}.")
