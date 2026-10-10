if(memix_ENABLE_CLANG_TIDY)
  set(CACHE{CLANGTIDY_EXTRA_ARGS} TYPE STRING HELP "Additional clang-tidy arguments (semicolon-separated)." VALUE "")
  find_program(CLANGTIDY clang-tidy REQUIRED)
endif()
if(memix_ENABLE_CPPCHECK)
  find_program(CPPCHECK cppcheck REQUIRED)
endif()

# Do not apply project analysis rules to third-party targets or Catch2 test macros.
function(memix_enable_static_analysis)
  foreach(target IN LISTS ARGN)
    if(memix_ENABLE_CLANG_TIDY)
      set_property(TARGET ${target} PROPERTY CXX_CLANG_TIDY
        "${CLANGTIDY}" -extra-arg=-Wno-unknown-warning-option ${CLANGTIDY_EXTRA_ARGS}
      )
      if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # These targets have no modules; clang-tidy cannot parse GCC's module flags.
        set_property(TARGET ${target} PROPERTY CXX_SCAN_FOR_MODULES OFF)
      endif()
    endif()
    if(memix_ENABLE_CPPCHECK)
      set_property(TARGET ${target} PROPERTY CXX_CPPCHECK
        "${CPPCHECK};--suppress=missingInclude;--enable=all;--inline-suppr;--inconclusive"
      )
    endif()
  endforeach()
endfunction()
