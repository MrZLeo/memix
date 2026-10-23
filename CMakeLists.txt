cmake_minimum_required(VERSION 4.4)
project(memix VERSION 0.7.0 LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not supported. Use cmake -S . -B build -G Ninja.")
endif()

# Initialize project targets and dependencies that do not choose their own standard.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/StandardSettings.cmake)
include(cmake/CompilerWarnings.cmake)
include(cmake/StaticAnalyzers.cmake)
include(cmake/Utils.cmake)
include(cmake/CPM.cmake)
include(cmake/SourcesAndHeaders.cmake)
include(GNUInstallDirs)

# Diagnose the removed mode rather than silently ignoring an old configuration.
if(memix_BUILD_HEADERS_ONLY)
  message(FATAL_ERROR "memix is a compiled library; memix_BUILD_HEADERS_ONLY is no longer supported.")
endif()

configure_file(cmake/version.hpp.in include/memix/version.hpp @ONLY)

set(memix_library_type)
if(memix_STATIC_LINK)
  set(memix_library_type STATIC)
endif()
add_library(memix ${memix_library_type} ${sources})
add_library(memix::memix ALIAS memix)
target_compile_features(memix PUBLIC cxx_std_23)
target_sources(memix
  PUBLIC FILE_SET HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}/include"
    FILES ${headers} "${CMAKE_CURRENT_BINARY_DIR}/include/memix/version.hpp"
  PRIVATE FILE_SET private_headers TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES ${private_headers}
)
set_target_properties(memix PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib/$<CONFIG>"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib/$<CONFIG>"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/$<CONFIG>"
  WINDOWS_EXPORT_ALL_SYMBOLS ON
  VERIFY_INTERFACE_HEADER_SETS ON
  VERIFY_PRIVATE_HEADER_SETS ON
)

# simdjson is compiled into memix, so it is not a downstream dependency.
CPMAddPackage(
  NAME simdjson
  GITHUB_REPOSITORY simdjson/simdjson
  GIT_TAG v5.0.3
  DOWNLOAD_ONLY YES
)
target_sources(memix PRIVATE "${simdjson_SOURCE_DIR}/singleheader/simdjson.cpp")
set_source_files_properties("${simdjson_SOURCE_DIR}/singleheader/simdjson.cpp" PROPERTIES
  COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>"
  SKIP_LINTING TRUE
  CXX_SCAN_FOR_MODULES OFF
)
# A build-only interface keeps vendor headers SYSTEM even in private header checks.
add_library(memix_simdjson_headers INTERFACE)
target_include_directories(memix_simdjson_headers SYSTEM INTERFACE "${simdjson_SOURCE_DIR}/singleheader")
target_link_libraries(memix PRIVATE "$<BUILD_INTERFACE:memix_simdjson_headers>")

set(bin_targets)
set(cli_targets)
if(memix_BUILD_EXECUTABLE)
  CPMAddPackage(
    NAME FTXUI
    GITHUB_REPOSITORY ArthurSonzogni/FTXUI
    VERSION 7.0.2
    SYSTEM YES
    OPTIONS
      "BUILD_SHARED_LIBS OFF"
      "FTXUI_BUILD_DOCS OFF"
      "FTXUI_BUILD_EXAMPLES OFF"
      "FTXUI_BUILD_TESTS OFF"
      "FTXUI_ENABLE_INSTALL OFF"
  )
  CPMAddPackage(
    NAME argum
    GITHUB_REPOSITORY gershnik/argum
    GIT_TAG v2.8
    GIT_SHALLOW TRUE
    SYSTEM YES
    OPTIONS "ARGUM_NO_TESTS ON"
  )
  CPMAddPackage(
    NAME spdlog
    GITHUB_REPOSITORY gabime/spdlog
    GIT_TAG v1.17.0
    SYSTEM YES
    OPTIONS
      "BUILD_SHARED_LIBS OFF"
      "SPDLOG_NO_EXCEPTIONS ON"
      "SPDLOG_BUILD_SHARED OFF"
      "SPDLOG_BUILD_EXAMPLE OFF"
      "SPDLOG_BUILD_TESTS OFF"
      "SPDLOG_INSTALL OFF"
  )

  # Keep command implementations modular behind the single memix entrypoint.
  add_library(memix_cli STATIC ${cli_sources} ${cli_headers})
  target_include_directories(memix_cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(memix_cli PRIVATE ARGUM_USE_EXPECTED=1 SPDLOG_NO_EXCEPTIONS=1)
  target_link_libraries(memix_cli PRIVATE
    memix::memix memix_simdjson_headers argum::argum spdlog::spdlog ftxui::component
  )
  list(APPEND cli_targets memix_cli)

  foreach(bin_source IN LISTS bin_sources)
    get_filename_component(bin_name "${bin_source}" NAME_WE)
    set(bin_target memix_${bin_name})
    add_executable(${bin_target} "${bin_source}")
    set_target_properties(${bin_target} PROPERTIES
      OUTPUT_NAME "${bin_name}"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/$<CONFIG>"
    )
    if(BUILD_SHARED_LIBS AND NOT memix_STATIC_LINK)
      file(RELATIVE_PATH memix_lib_from_bin "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
      if(APPLE)
        set_property(TARGET ${bin_target} PROPERTY INSTALL_RPATH "@loader_path/${memix_lib_from_bin}")
      elseif(UNIX)
        set_property(TARGET ${bin_target} PROPERTY INSTALL_RPATH "$ORIGIN/${memix_lib_from_bin}")
      endif()
    endif()
    target_include_directories(${bin_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${bin_target} PRIVATE memix_cli)
    if(memix_STATIC_LINK AND (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR MINGW))
      target_link_options(${bin_target} PRIVATE -static)
      set_target_properties(${bin_target} PROPERTIES
        LINK_SEARCH_START_STATIC TRUE
        LINK_SEARCH_END_STATIC TRUE
      )
    endif()
    list(APPEND bin_targets ${bin_target})
  endforeach()
endif()

# Exceptions stay enabled in Catch2/tests and the Objective-C++ bridge.
foreach(target IN ITEMS memix ${cli_targets} ${bin_targets})
  set_project_warnings(${target})
  if(MSVC)
    target_compile_options(${target} PRIVATE /EHs-c-)
    target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
  else()
    target_compile_options(${target} PRIVATE -fno-exceptions)
  endif()
endforeach()
memix_enable_static_analysis(memix ${cli_targets} ${bin_targets})

if(memix_ENABLE_CODE_COVERAGE)
  target_compile_options(memix PRIVATE -O0 -g --coverage)
  # Users of an instrumented library also need its coverage runtime.
  target_link_options(memix PUBLIC --coverage)
endif()

if(memix_BUILD_MACOS_INSPECTOR)
  if(NOT APPLE)
    message(FATAL_ERROR "memix_BUILD_MACOS_INSPECTOR is only supported on Apple platforms.")
  endif()
  set(CMAKE_OBJCXX_STANDARD 23)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
  set(CMAKE_OBJCXX_EXTENSIONS OFF)
  enable_language(OBJCXX)
  enable_language(Swift)
  add_subdirectory(macos/MemixInspector)
endif()

include(cmake/Doxygen.cmake)
add_clang_format_target()

if(memix_GENERATE_EXPORT_HEADER)
  include(GenerateExportHeader)
  generate_export_header(memix)
  target_sources(memix PUBLIC FILE_SET export_header TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_BINARY_DIR}"
    FILES "${CMAKE_CURRENT_BINARY_DIR}/memix_export.h"
  )
endif()

# Header sets provide the build include paths and the installed include layout.
install(TARGETS memix EXPORT memixTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILE_SET export_header DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
if(bin_targets)
  install(TARGETS ${bin_targets} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
install(EXPORT memixTargets NAMESPACE memix:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/memix")

include(CMakePackageConfigHelpers)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/memixConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SemanticVersion
)
configure_package_config_file(cmake/memixConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/memixConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/memix"
  PATH_VARS CMAKE_INSTALL_INCLUDEDIR
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/memixConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/memixConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/memix"
)

if(memix_ENABLE_UNIT_TESTING)
  enable_testing()
  add_subdirectory(test)
endif()
