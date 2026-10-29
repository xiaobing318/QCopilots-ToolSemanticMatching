# Every distribution contains the whole application. Component installation
# would include EXCLUDE_FROM_ALL developer exports and could sign the macOS
# bundle before a later component writes runtime resources into it.
set(CPACK_MONOLITHIC_INSTALL ON)
if(DEFINED CPACK_QCTSM_SOURCE_DIR)
  set(Python3_EXECUTABLE "${CPACK_QCTSM_PYTHON_EXECUTABLE}")
  include("${CMAKE_CURRENT_LIST_DIR}/qctsm-package-paths.cmake")
  foreach(_root CPACK_PACKAGE_DIRECTORY CPACK_TOPLEVEL_DIRECTORY)
    if(DEFINED ${_root} AND NOT "${${_root}}" STREQUAL "")
      qctsm_package_require_external_path("${_root}" "${${_root}}" "${CPACK_QCTSM_SOURCE_DIR}")
    endif()
  endforeach()
endif()

if(CPACK_GENERATOR MATCHES "^NSIS")
  # CMake 3.28's built-in template unconditionally asks for administrator
  # access. Our small template installs only for the current Windows user.
  list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/nsis")
  set(CPACK_NSIS_INSTALL_ROOT "$LOCALAPPDATA/Programs")
  set(CPACK_NSIS_EXECUTION_LEVEL user)
  # CPack writes UTF-8 scripts without a BOM. NSIS otherwise interprets paths
  # using the host ANSI code page, breaking a checkout containing Chinese text.
  set(CPACK_NSIS_EXECUTABLE_PRE_ARGUMENTS "INPUTCHARSET UTF8")
elseif(CPACK_GENERATOR STREQUAL "DEB")
  set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/qctsm")
  if(CPACK_QCTSM_SYSTEM_NAME STREQUAL "Linux" AND CPACK_QCTSM_GPU_BACKEND STREQUAL "cuda")
    # CUDA's optional backend references the display driver's SONAME. Analyze
    # it using an SDK stub copy outside every payload, never a runtime path.
    set(_qctsm_analysis_config "${CPACK_BUILD_CONFIG}")
    if(_qctsm_analysis_config STREQUAL "")
      set(_qctsm_analysis_config "${CPACK_QCTSM_BUILD_TYPE}")
    endif()
    if(NOT _qctsm_analysis_config MATCHES "^[A-Za-z0-9_.+-]+$")
      message(FATAL_ERROR "CUDA DEB analysis requires an explicit build configuration")
    endif()
    if(NOT CPACK_QCTSM_VARIANT MATCHES "^linux-x64-cuda-[0-9]+\\.[0-9]+$")
      message(FATAL_ERROR "CUDA DEB analysis requires a Linux x64 CUDA variant")
    endif()
    if(NOT IS_ABSOLUTE "${CPACK_QCTSM_BUILD_DIR}" OR NOT EXISTS "${CPACK_QCTSM_GPU_SDK_MANIFEST}")
      message(FATAL_ERROR "CUDA DEB analysis requires the external build directory and verified SDK manifest")
    endif()
    set(_qctsm_analysis "${CPACK_QCTSM_BUILD_DIR}/qctsm-deb-analysis/${CPACK_QCTSM_VARIANT}/${_qctsm_analysis_config}")
    qctsm_package_require_external_path(CPACK_QCTSM_DEB_ANALYSIS_DIRECTORY "${_qctsm_analysis}" "${CPACK_QCTSM_SOURCE_DIR}")
    set(_qctsm_analysis_paths "${_qctsm_analysis}")
    set(_qctsm_analysis_boundaries)
    foreach(_qctsm_boundary CPACK_QCTSM_INSTALL_PREFIX CPACK_PACKAGE_DIRECTORY CPACK_TOPLEVEL_DIRECTORY CPACK_TEMPORARY_DIRECTORY)
      if(DEFINED ${_qctsm_boundary} AND NOT "${${_qctsm_boundary}}" STREQUAL "")
        list(APPEND _qctsm_analysis_paths "${${_qctsm_boundary}}")
        list(APPEND _qctsm_analysis_boundaries "${_qctsm_boundary}")
      endif()
    endforeach()
    # REAL_PATH can preserve the lexical input when its final component does
    # not exist. pathlib also resolves existing linked ancestors of new paths.
    execute_process(COMMAND "${CPACK_QCTSM_PYTHON_EXECUTABLE}" -B -c
      "import json,os,pathlib,sys; p=[pathlib.Path(s).resolve().as_posix() for s in sys.argv[1:]]; print(json.dumps([s.lower() if os.name == 'nt' else s for s in p]))"
      ${_qctsm_analysis_paths}
      RESULT_VARIABLE _qctsm_paths_status OUTPUT_VARIABLE _qctsm_resolved_paths ERROR_VARIABLE _qctsm_paths_error)
    if(NOT _qctsm_paths_status EQUAL 0)
      message(FATAL_ERROR "Cannot resolve CUDA DEB analysis boundaries: ${_qctsm_paths_error}")
    endif()
    string(JSON _qctsm_analysis GET "${_qctsm_resolved_paths}" 0)
    set(_qctsm_boundary_index 0)
    foreach(_qctsm_boundary IN LISTS _qctsm_analysis_boundaries)
      math(EXPR _qctsm_boundary_index "${_qctsm_boundary_index} + 1")
      string(JSON _qctsm_payload_root GET "${_qctsm_resolved_paths}" ${_qctsm_boundary_index})
      cmake_path(IS_PREFIX _qctsm_payload_root "${_qctsm_analysis}" NORMALIZE _qctsm_inside_payload)
      cmake_path(IS_PREFIX _qctsm_analysis "${_qctsm_payload_root}" NORMALIZE _qctsm_contains_payload)
      if(_qctsm_inside_payload OR _qctsm_contains_payload)
        message(FATAL_ERROR "CUDA driver analysis directory overlaps ${_qctsm_boundary}: ${_qctsm_analysis}")
      endif()
    endforeach()
    execute_process(COMMAND "${CPACK_QCTSM_PYTHON_EXECUTABLE}" -B "${CPACK_QCTSM_SOURCE_DIR}/tools/qctsm-gpu-runtime.py"
      --variant "${CPACK_QCTSM_VARIANT}" --sdk-manifest "${CPACK_QCTSM_GPU_SDK_MANIFEST}"
      --deb-analysis-directory "${_qctsm_analysis}"
      RESULT_VARIABLE _qctsm_analysis_status OUTPUT_VARIABLE _qctsm_analysis_output ERROR_VARIABLE _qctsm_analysis_error)
    if(NOT _qctsm_analysis_status EQUAL 0)
      message(FATAL_ERROR "CUDA DEB driver analysis preparation failed:\n${_qctsm_analysis_output}\n${_qctsm_analysis_error}")
    endif()
    message(STATUS "CUDA DEB driver analysis: ${_qctsm_analysis_output}")
    list(APPEND CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS "${_qctsm_analysis}")
    list(APPEND CPACK_POST_BUILD_SCRIPTS "${CMAKE_CURRENT_LIST_DIR}/qctsm-deb-verify.cmake")
  endif()
endif()
