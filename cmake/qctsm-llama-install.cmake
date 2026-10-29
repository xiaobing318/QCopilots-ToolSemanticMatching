include_guard(GLOBAL)

function(qctsm_install_inference_runtime)
  cmake_parse_arguments(PARSE_ARGV 0 _runtime "" "" "LIBRARIES;CPU_BACKENDS;GPU_BACKENDS")
  if(_runtime_LIBRARIES)
    install(TARGETS ${_runtime_LIBRARIES}
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
      LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
      ARCHIVE EXCLUDE_FROM_ALL)
  endif()
  set(_backends ${_runtime_CPU_BACKENDS} ${_runtime_GPU_BACKENDS})
  if(_backends)
    set(_backend_directory "${CMAKE_INSTALL_LIBDIR}")
    if(APPLE)
      cmake_path(GET CMAKE_INSTALL_BINDIR PARENT_PATH _bundle_contents)
      set(_backend_directory "${_bundle_contents}/PlugIns/ggml")
      # CPU modules are loaded with dlopen and must be inside macdeployqt's
      # signed PlugIns tree. Their linked base libraries stay in Frameworks.
      set_target_properties(${_backends} PROPERTIES
        INSTALL_RPATH "@loader_path/../../Frameworks")
    endif()
    foreach(_backend IN LISTS _backends)
      get_target_property(_imported "${_backend}" IMPORTED)
      if(_imported)
        # A separately compiled Windows GPU module still belongs beside the
        # application's canonical ggml-base, never its subbuild's duplicate.
        install(FILES "$<TARGET_FILE:${_backend}>" DESTINATION "${CMAKE_INSTALL_BINDIR}")
      else()
        set(_module_directory "${_backend_directory}")
        if(WIN32 AND NOT APPLE AND _backend IN_LIST _runtime_GPU_BACKENDS)
          set(_module_directory "${CMAKE_INSTALL_BINDIR}")
        endif()
        if(UNIX AND NOT APPLE AND _backend IN_LIST _runtime_GPU_BACKENDS)
          set_property(TARGET "${_backend}" APPEND PROPERTY INSTALL_RPATH
            "$ORIGIN" "$ORIGIN/rocm/_rocm_sdk_core/lib" "$ORIGIN/rocm/_rocm_sdk_libraries/lib")
        endif()
        install(TARGETS "${_backend}"
          RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
          LIBRARY DESTINATION "${_module_directory}"
          ARCHIVE EXCLUDE_FROM_ALL)
      endif()
    endforeach()
  endif()
endfunction()
