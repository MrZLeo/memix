function(project_detect_cxx_standard out_var)
  set(_project_cxx_standard 23)
  message(STATUS "Using C++${_project_cxx_standard}")
  set(${out_var} ${_project_cxx_standard} PARENT_SCOPE)
endfunction()

function(project_apply_cxx_standard target visibility)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "Cannot apply the project C++ standard to unknown target `${target}`.")
  endif()

  set_target_properties(
    ${target}
    PROPERTIES
      CXX_STANDARD ${PROJECT_CXX_STANDARD}
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF
  )

  target_compile_features(${target} ${visibility} cxx_std_${PROJECT_CXX_STANDARD})
endfunction()
