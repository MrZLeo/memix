if(${PROJECT_NAME}_ENABLE_CLANG_TIDY)
  find_program(CLANGTIDY clang-tidy)
  if(CLANGTIDY)
    set(PROJECT_CLANG_TIDY_COMMAND
        ${CLANGTIDY}
        -extra-arg=-Wno-unknown-warning-option
    )
    message("Clang-Tidy finished setting up.")
  else()
    message(SEND_ERROR "Clang-Tidy requested but executable not found.")
  endif()
endif()

function(project_enable_clang_tidy_for_targets)
  if(NOT PROJECT_CLANG_TIDY_COMMAND)
    return()
  endif()

  foreach(target_name IN LISTS ARGN)
    if(TARGET ${target_name})
      set_target_properties(
        ${target_name}
        PROPERTIES
          CXX_CLANG_TIDY "${PROJECT_CLANG_TIDY_COMMAND}"
      )
    endif()
  endforeach()
endfunction()

if(${PROJECT_NAME}_ENABLE_CPPCHECK)
  find_program(CPPCHECK cppcheck)
  if(CPPCHECK)
    set(CMAKE_CXX_CPPCHECK ${CPPCHECK} --suppress=missingInclude --enable=all
                           --inline-suppr --inconclusive -i ${CMAKE_SOURCE_DIR}/imgui/lib)
    message("Cppcheck finished setting up.")
  else()
    message(SEND_ERROR "Cppcheck requested but executable not found.")
  endif()
endif()
