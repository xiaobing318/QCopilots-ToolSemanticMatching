# CPack runs this after generating the DEB. A policy failure makes CPack fail,
# so qctsm-dev never promotes this invocation's private output to verified packages.
if(NOT CPACK_PACKAGE_FILES)
  message(FATAL_ERROR "CUDA DEB verification did not receive a generated package")
endif()
foreach(_qctsm_package IN LISTS CPACK_PACKAGE_FILES)
  if(NOT _qctsm_package MATCHES "\\.deb$" OR NOT EXISTS "${_qctsm_package}")
    message(FATAL_ERROR "CUDA DEB verification received an unexpected package: ${_qctsm_package}")
  endif()
  execute_process(COMMAND "${CPACK_QCTSM_PYTHON_EXECUTABLE}" -B "${CPACK_QCTSM_SOURCE_DIR}/tools/qctsm-gpu-runtime.py"
    --variant "${CPACK_QCTSM_VARIANT}" --verify-deb "${_qctsm_package}"
    RESULT_VARIABLE _qctsm_verify_status OUTPUT_VARIABLE _qctsm_verify_output ERROR_VARIABLE _qctsm_verify_error)
  if(NOT _qctsm_verify_status EQUAL 0)
    message(FATAL_ERROR "CUDA DEB driver policy failed:\n${_qctsm_verify_output}\n${_qctsm_verify_error}")
  endif()
  message(STATUS "CUDA DEB driver policy: ${_qctsm_verify_output}")
endforeach()
