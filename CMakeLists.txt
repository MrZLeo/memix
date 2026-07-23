cmake_minimum_required(VERSION 3.25)

#
# Project details
#

project(
  "memix"
  VERSION 0.1.1
  LANGUAGES CXX
)

option(${PROJECT_NAME}_BUILD_MACOS_INSPECTOR "Build the native macOS Memix Inspector app." OFF)

#
# Set project options
#
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "") # works

include(cmake/StandardSettings.cmake)
include(cmake/StaticAnalyzers.cmake)
include(cmake/Utils.cmake)
include(cmake/CPM.cmake)
include(cmake/ProjectStandard.cmake)

project_detect_cxx_standard(PROJECT_CXX_STANDARD)
if(${PROJECT_NAME}_BUILD_HEADERS_ONLY)
  message(
    FATAL_ERROR
    "${PROJECT_NAME}_BUILD_HEADERS_ONLY is not supported because memix now has compiled store sources. "
    "Configure with -D${PROJECT_NAME}_BUILD_HEADERS_ONLY=OFF."
  )
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug")
endif()
message(STATUS "Started CMake for ${PROJECT_NAME} v${PROJECT_VERSION}...\n")

# CMake uses this variable to initialize every target as it is created, so it
# must be set before memix and its dependencies define their targets.
if(MSVC)
  if(${PROJECT_NAME}_STATIC_LINK)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  endif()
endif()

if(${PROJECT_NAME}_STATIC_LINK)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Static linking enabled: Linux executables will have no dynamic runtime dependencies.")
  elseif(MSVC)
    message(STATUS "Static linking enabled: Windows executables will use the static MSVC runtime.")
  elseif(MINGW)
    message(STATUS "Static linking enabled: MinGW executables will have no dynamic runtime dependencies.")
  elseif(APPLE)
    message(STATUS "Static linking enabled for bundled code; macOS system libraries remain dynamic.")
  else()
    message(WARNING "${PROJECT_NAME}_STATIC_LINK is not implemented for ${CMAKE_SYSTEM_NAME}; using the platform default runtime linkage.")
  endif()
endif()

if (UNIX)
    add_compile_options("$<$<CONFIG:DEBUG>:-D_DEBUG>")    #this will allow to use same _DEBUG macro available in both Linux as well as Windows - MSCV environment. Easy to put Debug specific code.
endif (UNIX)


#
# Setup alternative names
#

if(${PROJECT_NAME}_USE_ALT_NAMES)
  string(TOLOWER ${PROJECT_NAME} PROJECT_NAME_LOWERCASE)
  string(TOUPPER ${PROJECT_NAME} PROJECT_NAME_UPPERCASE)
else()
  set(PROJECT_NAME_LOWERCASE ${PROJECT_NAME})
  set(PROJECT_NAME_UPPERCASE ${PROJECT_NAME})
endif()

#
# Prevent building in the source directory
#

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there.\n")
endif()

#
# Create library, setup header and source files
#

# Find all headers and implementation files
include(cmake/SourcesAndHeaders.cmake)

if(${PROJECT_NAME}_BUILD_HEADERS_ONLY)
  add_library(${PROJECT_NAME} INTERFACE)

  if(${PROJECT_NAME}_VERBOSE_OUTPUT)
    verbose_message("Found the following headers:")
    foreach(header IN LISTS headers)
      verbose_message("* ${header}")
    endforeach()
  endif()
else()
  set(memix_library_type)
  if(${PROJECT_NAME}_STATIC_LINK)
    set(memix_library_type STATIC)
  endif()
  add_library(
    ${PROJECT_NAME}
    ${memix_library_type}
    ${headers}
    ${private_headers}
    ${sources}
  )

  if(${PROJECT_NAME}_VERBOSE_OUTPUT)
    verbose_message("Found the following sources:")
    foreach(source IN LISTS sources)
      verbose_message("* ${source}")
    endforeach()
    verbose_message("Found the following headers:")
    foreach(header IN LISTS headers)
      verbose_message("* ${header}")
    endforeach()
    verbose_message("Found the following private headers:")
    foreach(header IN LISTS private_headers)
      verbose_message("* ${header}")
    endforeach()
  endif()
endif()

set(bin_targets)
if(${PROJECT_NAME}_BUILD_EXECUTABLE)
  if(${PROJECT_NAME}_VERBOSE_OUTPUT)
    verbose_message("Found the following executable entrypoints:")
    foreach(bin_source IN LISTS bin_sources)
      verbose_message("* ${bin_source}")
    endforeach()
  endif()

  foreach(bin_source IN LISTS bin_sources)
    get_filename_component(bin_name ${bin_source} NAME_WE)
    set(bin_target ${PROJECT_NAME}_${bin_name})

    add_executable(${bin_target} ${bin_source})
    set_target_properties(
      ${bin_target}
      PROPERTIES
      OUTPUT_NAME ${bin_name}
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}"
    )

    if(${PROJECT_NAME}_STATIC_LINK)
      if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR MINGW)
        target_link_options(${bin_target} PRIVATE -static)
        set_target_properties(
          ${bin_target}
          PROPERTIES
          LINK_SEARCH_START_STATIC TRUE
          LINK_SEARCH_END_STATIC TRUE
        )
      endif()
    endif()

    list(APPEND bin_targets ${bin_target})
  endforeach()
endif()

set_target_properties(
  ${PROJECT_NAME}
  PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE}"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE}"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}"
)

message(STATUS "Added all header and implementation files.\n")

project_enable_clang_tidy_for_targets(${PROJECT_NAME} ${bin_targets})

#
# Set the project standard and warnings
#

if(${PROJECT_NAME}_BUILD_HEADERS_ONLY)
  project_apply_cxx_standard(${PROJECT_NAME} INTERFACE)
else()
  project_apply_cxx_standard(${PROJECT_NAME} PUBLIC)
endif()
foreach(bin_target IN LISTS bin_targets)
  project_apply_cxx_standard(${bin_target} PRIVATE)
endforeach()
include(cmake/CompilerWarnings.cmake)
set_project_warnings(${PROJECT_NAME})
verbose_message("Applied compiler warnings. Using standard C++${PROJECT_CXX_STANDARD}.\n")

function(memix_disable_exceptions target)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "Cannot disable exceptions for unknown target `${target}`.")
  endif()

  if(MSVC)
    target_compile_options(${target} PRIVATE /EHs-c-)
    target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
  else()
    target_compile_options(${target} PRIVATE -fno-exceptions)
  endif()
endfunction()

function(memix_enable_msvc_conforming_preprocessor target)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "Cannot enable the MSVC conforming preprocessor for unknown target `${target}`.")
  endif()

  target_compile_options(${target} PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/Zc:preprocessor>")
endfunction()

if(NOT ${PROJECT_NAME}_BUILD_HEADERS_ONLY)
  memix_disable_exceptions(${PROJECT_NAME})
endif()
foreach(bin_target IN LISTS bin_targets)
  memix_disable_exceptions(${bin_target})
  memix_enable_msvc_conforming_preprocessor(${bin_target})
endforeach()

#
# Enable Doxygen
#

include(cmake/Doxygen.cmake)

#
# Model project dependencies
#

CPMAddPackage(
  NAME simdjson
  GITHUB_REPOSITORY simdjson/simdjson
  GIT_TAG v4.6.2
  DOWNLOAD_ONLY YES
)
target_sources(
  ${PROJECT_NAME}
  PRIVATE
    ${simdjson_SOURCE_DIR}/singleheader/simdjson.cpp
)

set_source_files_properties(
  ${simdjson_SOURCE_DIR}/singleheader/simdjson.cpp
  PROPERTIES
    COMPILE_OPTIONS "-w"
    SKIP_LINTING TRUE
)

target_include_directories(
  ${PROJECT_NAME}
  SYSTEM PRIVATE
    ${simdjson_SOURCE_DIR}/singleheader
)

# Identify and link with the specific "packages" the project uses
if(${PROJECT_NAME}_BUILD_EXECUTABLE)
  CPMAddPackage(
    NAME argum
    GITHUB_REPOSITORY gershnik/argum
    GIT_TAG v2.8
    GIT_SHALLOW TRUE
    OPTIONS
      "ARGUM_NO_TESTS ON"
  )
  CPMAddPackage(
    NAME spdlog
    GITHUB_REPOSITORY gabime/spdlog
    GIT_TAG v1.17.0
    OPTIONS
      "SPDLOG_NO_EXCEPTIONS ON"
      "SPDLOG_BUILD_SHARED OFF"
      "SPDLOG_BUILD_EXAMPLE OFF"
      "SPDLOG_BUILD_TESTS OFF"
      "SPDLOG_INSTALL OFF"
  )

  if(NOT TARGET argum::argum)
    find_package(argum CONFIG REQUIRED)
  endif()
  if(NOT TARGET spdlog::spdlog)
    find_package(spdlog CONFIG REQUIRED)
  endif()

  foreach(bin_target IN LISTS bin_targets)
    target_include_directories(
      ${bin_target}
      PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if(DEFINED argum_SOURCE_DIR)
      target_include_directories(
        ${bin_target}
        SYSTEM PRIVATE
          ${argum_SOURCE_DIR}/inc
      )
    endif()
    target_include_directories(
      ${bin_target}
      SYSTEM PRIVATE
        ${simdjson_SOURCE_DIR}/singleheader
    )
    target_compile_definitions(
      ${bin_target}
      PRIVATE
        ARGUM_USE_EXPECTED=1
        SPDLOG_NO_EXCEPTIONS=1
    )
    target_link_libraries(
      ${bin_target}
      PRIVATE
        ${PROJECT_NAME}
        spdlog::spdlog
    )
    if(NOT DEFINED argum_SOURCE_DIR)
      target_link_libraries(${bin_target} PRIVATE argum::argum)
    endif()
  endforeach()
endif()

verbose_message("Successfully added all dependencies and linked against them.")

#
# Set the build/user include directories
#

# Allow usage of header files in the `src` directory, but only for utilities
if(${PROJECT_NAME}_BUILD_HEADERS_ONLY)
  target_include_directories(
    ${PROJECT_NAME}
    INTERFACE
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  )
else()
  target_include_directories(
    ${PROJECT_NAME}
    PUBLIC
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
endif()

message(STATUS "Finished setting up include directories.")

#
# Provide alias to library for
#

add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

verbose_message("Project is now aliased as ${PROJECT_NAME}::${PROJECT_NAME}.\n")

if(${PROJECT_NAME}_BUILD_MACOS_INSPECTOR)
  if(NOT APPLE)
    message(FATAL_ERROR "memix_BUILD_MACOS_INSPECTOR is only supported on Apple platforms.")
  endif()
  enable_language(OBJCXX)
  if(POLICY CMP0157)
    cmake_policy(SET CMP0157 NEW)
  endif()
  enable_language(Swift)
  add_subdirectory(macos/MemixInspector)
endif()

#
# Format the project using the `clang-format` target (i.e: cmake --build build --target clang-format)
#

add_clang_format_target()

#
# Install library for easy downstream inclusion
#

include(GNUInstallDirs)
install(
  TARGETS
  ${PROJECT_NAME}
  EXPORT
  ${PROJECT_NAME}Targets
  LIBRARY DESTINATION
  ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION
  ${CMAKE_INSTALL_BINDIR}
  ARCHIVE DESTINATION
  ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION
  include
  PUBLIC_HEADER DESTINATION
  include
)

if(bin_targets)
  install(
    TARGETS
    ${bin_targets}
    RUNTIME DESTINATION
    ${CMAKE_INSTALL_BINDIR}
  )
endif()

install(
  EXPORT
  ${PROJECT_NAME}Targets
  FILE
  ${PROJECT_NAME}Targets.cmake
  NAMESPACE
  ${PROJECT_NAME}::
  DESTINATION
  ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

#
# Add version header
#

configure_file(
  ${CMAKE_CURRENT_LIST_DIR}/cmake/version.hpp.in
  include/${PROJECT_NAME_LOWERCASE}/version.hpp
  @ONLY
)

install(
  FILES
  ${CMAKE_CURRENT_BINARY_DIR}/include/${PROJECT_NAME_LOWERCASE}/version.hpp
  DESTINATION
  include/${PROJECT_NAME_LOWERCASE}
)

#
# Install the `include` directory
#

install(
  DIRECTORY
  include/${PROJECT_NAME_LOWERCASE}
  DESTINATION
  include
)

verbose_message("Install targets successfully built. Install with `cmake --build <build_directory> --target install --config <build_config>`.")

#
# Quick `ConfigVersion.cmake` creation
#

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  ${PROJECT_NAME}ConfigVersion.cmake
  VERSION
  ${PROJECT_VERSION}
  COMPATIBILITY
  SameMajorVersion
)

configure_package_config_file(
  ${CMAKE_CURRENT_LIST_DIR}/cmake/${PROJECT_NAME}Config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
  INSTALL_DESTINATION
  ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

install(
  FILES
  ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
  DESTINATION
  ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

#
# Generate export header if specified
#

if(${PROJECT_NAME}_GENERATE_EXPORT_HEADER)
  include(GenerateExportHeader)
  generate_export_header(${PROJECT_NAME})
  install(
    FILES
    ${PROJECT_BINARY_DIR}/${PROJECT_NAME_LOWERCASE}_export.h
    DESTINATION
    include
  )

  message(STATUS "Generated the export header `${PROJECT_NAME_LOWERCASE}_export.h` and installed it.")
endif()

message(STATUS "Finished building requirements for installing the package.\n")

#
# Unit testing setup
#

if(${PROJECT_NAME}_ENABLE_UNIT_TESTING)
  enable_testing()
  message(STATUS "Build unit tests for the project. Tests should always be found in the test folder\n")
  add_subdirectory(test)
endif()
