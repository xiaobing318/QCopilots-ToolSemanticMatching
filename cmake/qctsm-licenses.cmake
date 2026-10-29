# Runtime notice collection adapted from QCopilots-Search (MIT).
include_guard(GLOBAL)
include(FetchContent)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

set(QCOPILOTS_TOOL_SEMANTIC_MATCHING_LICENSE_SOURCE_CACHE
  "${QCOPILOTS_TOOL_SEMANTIC_MATCHING_DEPENDENCY_ROOT}/licenses"
  CACHE PATH "External cache for hash-locked Qt license source archives")
qctsm_require_external_path(QCOPILOTS_TOOL_SEMANTIC_MATCHING_LICENSE_SOURCE_CACHE
  "${QCOPILOTS_TOOL_SEMANTIC_MATCHING_LICENSE_SOURCE_CACHE}")
option(QCOPILOTS_TOOL_SEMANTIC_MATCHING_DOWNLOAD_LICENSE_SOURCES
  "Download missing hash-locked Qt license source archives" ON)
get_filename_component(_qctsm_license_qt_prefix "${Qt6Core_DIR}/../../.." ABSOLUTE)
FetchContent_GetProperties(qctsm_llama SOURCE_DIR _qctsm_license_llama_source)
if(NOT _qctsm_license_llama_source)
  message(FATAL_ERROR "Verified llama.cpp source is required for its runtime license materials")
endif()

set(_qctsm_license_tool "${PROJECT_SOURCE_DIR}/tools/qctsm-license-materials.py")
set(_qctsm_license_lock "${PROJECT_SOURCE_DIR}/tools/qctsm-source-packages.lock.json")
set(_qctsm_license_output "${CMAKE_BINARY_DIR}/gen/licenses")
set(_qctsm_license_download)
if(QCOPILOTS_TOOL_SEMANTIC_MATCHING_DOWNLOAD_LICENSE_SOURCES)
  set(_qctsm_license_download --download)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_qctsm_license_tool}" "${_qctsm_license_lock}"
  "${_qctsm_license_llama_source}/LICENSE"
  "${_qctsm_license_llama_source}/vendor/cpp-httplib/LICENSE")
execute_process(COMMAND "${Python3_EXECUTABLE}" -X utf8 -B "${_qctsm_license_tool}"
  --qt-prefix "${_qctsm_license_qt_prefix}" --llama-source "${_qctsm_license_llama_source}"
  --cache-dir "${QCOPILOTS_TOOL_SEMANTIC_MATCHING_LICENSE_SOURCE_CACHE}"
  --output-dir "${_qctsm_license_output}" --source-lock "${_qctsm_license_lock}"
  ${_qctsm_license_download}
  RESULT_VARIABLE _qctsm_license_result OUTPUT_VARIABLE _qctsm_license_stdout
  ERROR_VARIABLE _qctsm_license_stderr)
if(NOT _qctsm_license_result EQUAL 0)
  message(FATAL_ERROR "Cannot prepare complete Qt/llama/ggml/httplib licenses:\n${_qctsm_license_stdout}${_qctsm_license_stderr}")
endif()
message(STATUS "Runtime license materials: ${_qctsm_license_stdout}")

# GPU deployment is registered by qctsm_finalize_llama before this module.
# Collect only notices for the runtime files actually placed in this payload.
if(QCOPILOTS_TOOL_SEMANTIC_MATCHING_VARIANT MATCHES "-(cuda-[0-9.]+|rocm|vulkan)$")
  install(CODE "set(_qctsm_gpu_license_manifest \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/qctsm-gpu-runtime.json\")
if(NOT EXISTS \"\${_qctsm_gpu_license_manifest}\")
  message(FATAL_ERROR \"Deployed GPU runtime license manifest is missing: \${_qctsm_gpu_license_manifest}\")
endif()
execute_process(COMMAND \"${Python3_EXECUTABLE}\" -X utf8 -B \"${_qctsm_license_tool}\"
  --qt-prefix \"${_qctsm_license_qt_prefix}\" --llama-source \"${_qctsm_license_llama_source}\"
  --cache-dir \"${QCOPILOTS_TOOL_SEMANTIC_MATCHING_LICENSE_SOURCE_CACHE}\"
  --output-dir \"${_qctsm_license_output}\" --source-lock \"${_qctsm_license_lock}\"
  --runtime-license-manifest \"\${_qctsm_gpu_license_manifest}\"
  RESULT_VARIABLE _qctsm_gpu_license_result OUTPUT_VARIABLE _qctsm_gpu_license_output ERROR_VARIABLE _qctsm_gpu_license_error)
if(NOT _qctsm_gpu_license_result EQUAL 0)
  message(FATAL_ERROR \"Cannot collect deployed GPU license materials: \${_qctsm_gpu_license_output}\${_qctsm_gpu_license_error}\")
endif()")
endif()
install(CODE "execute_process(COMMAND \"${Python3_EXECUTABLE}\" -X utf8 -B \"${_qctsm_license_tool}\"
  --verify \"${_qctsm_license_output}\" RESULT_VARIABLE _qctsm_license_result
  OUTPUT_VARIABLE _qctsm_license_output ERROR_VARIABLE _qctsm_license_error)
if(NOT _qctsm_license_result EQUAL 0)
  message(FATAL_ERROR \"Runtime license validation failed: \${_qctsm_license_output}\${_qctsm_license_error}\")
endif()")
install(DIRECTORY "${_qctsm_license_output}/"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/qctsm/licenses/qctsm-runtime")

if(QCOPILOTS_TOOL_SEMANTIC_MATCHING_BUILD_TESTS)
  add_test(NAME qctsm-license-materials COMMAND "${Python3_EXECUTABLE}" -X utf8 -B
    "${PROJECT_SOURCE_DIR}/.github/scripts/test_qctsm_licenses.py" --materials "${_qctsm_license_output}")
  set_tests_properties(qctsm-license-materials PROPERTIES LABELS "package;licenses" TIMEOUT 60)
endif()
