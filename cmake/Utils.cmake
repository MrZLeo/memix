# Format all owned C++ files, including disabled CLI/test targets, but not vendors
# or generated headers. Override memix_CLANG_FORMAT_BINARY to choose a formatter.
function(add_clang_format_target)
  find_program(memix_CLANG_FORMAT_BINARY clang-format)
  if(memix_CLANG_FORMAT_BINARY)
    add_custom_target(clang-format
      COMMAND "${memix_CLANG_FORMAT_BINARY}" -i
        ${sources} ${headers} ${private_headers} ${bin_sources} ${bin_headers} ${test_sources}
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      COMMAND_EXPAND_LISTS
      VERBATIM
    )
  endif()
endfunction()
