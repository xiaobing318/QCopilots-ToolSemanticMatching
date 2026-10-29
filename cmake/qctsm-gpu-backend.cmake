include_guard(GLOBAL)
include(ExternalProject)

# Windows upstream builds CUDA/HIP modules separately, then uses the common
# CPU package's ggml-base. Keep the compiler boundary at the ggml C ABI.
function(qctsm_add_windows_gpu_backend source_dir output)
  if(NOT WIN32 OR NOT QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_BACKEND MATCHES "^(cuda|hip)$")
    set(${output} "" PARENT_SCOPE)
    return()
  endif()
  set(QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_ROOT "" CACHE PATH "Verified GPU SDK root produced by qctsm-gpu-sdk.py")
  set(_backend "${QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_BACKEND}")
  # The parent build already isolates variant and configuration. Avoid repeating
  # that identity in long upstream CUDA/HIP template object paths.
  set(_build "${CMAKE_BINARY_DIR}/nd/gpu")
  qctsm_require_external_path(QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_BINARY_DIR "${_build}")
  set(_args
    -DCMAKE_BUILD_TYPE=$<CONFIG>
    "-DLLAMA_BUILD_COMMIT=${LLAMA_BUILD_COMMIT}"
    "-DGIT_EXE=OFF"
    -DBUILD_SHARED_LIBS=ON -DGGML_BACKEND_DL=ON -DGGML_NATIVE=OFF -DGGML_CPU=OFF -DGGML_CCACHE=OFF
    -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_EXAMPLES=OFF
    -DLLAMA_BUILD_MTMD=OFF -DLLAMA_BUILD_COMMON=OFF
    -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_APP=OFF -DLLAMA_BUILD_UI=OFF)
  if(DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    list(APPEND _args "-DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}")
  endif()
  if(_backend STREQUAL "hip")
    if(QCOPILOTS_TOOL_SEMANTIC_MATCHING_HIP_ROOT)
      set(QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_ROOT "${QCOPILOTS_TOOL_SEMANTIC_MATCHING_HIP_ROOT}")
    endif()
    if(NOT QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_ROOT AND DEFINED ENV{HIP_PATH})
      set(QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_ROOT "$ENV{HIP_PATH}" CACHE PATH "Verified HIP SDK root" FORCE)
    endif()
    if(NOT EXISTS "${QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_ROOT}/lib/llvm/bin/clang++.exe")
      message(FATAL_ERROR "Windows ROCm requires a prepared ROCm ${QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_TOOLKIT_VERSION} SDK. Run tools/qctsm-gpu-sdk.py and pass QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_ROOT.")
    endif()
    string(REPLACE ";" "|" _gpu_targets "${QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_TARGETS}")
    list(APPEND _args -DGGML_HIP=ON
      "-DCMAKE_PREFIX_PATH=${QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_ROOT}"
      "-DHIP_PATH=${QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_ROOT}"
      "-DCMAKE_C_COMPILER=${QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_ROOT}/lib/llvm/bin/clang.exe"
      "-DCMAKE_CXX_COMPILER=${QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_ROOT}/lib/llvm/bin/clang++.exe"
      "-DCMAKE_HIP_COMPILER=${QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_ROOT}/lib/llvm/bin/clang.exe"
      "-DCMAKE_C_FLAGS=-Wno-error=incompatible-pointer-types"
      "-DAMDGPU_TARGETS=${_gpu_targets}")
  else()
    list(APPEND _args -DGGML_CUDA=ON -DGGML_CUDA_CCCL_VERSION=v3.4.3)
    if(CUDAToolkit_ROOT)
      set(QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_ROOT "${CUDAToolkit_ROOT}")
    endif()
    if(QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_ROOT)
      list(APPEND _args "-DCUDAToolkit_ROOT=${QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_ROOT}"
        "-DCMAKE_CUDA_COMPILER=${QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_ROOT}/bin/nvcc.exe")
    endif()
    # Deliberately leave CUDA architectures unset, as in the pinned recipe.
  endif()
  set(_module "${_build}/bin/$<CONFIG>/ggml-${_backend}.dll")
  ExternalProject_Add(qctsm-gpu-backend-build
    SOURCE_DIR "${source_dir}" BINARY_DIR "${_build}"
    LIST_SEPARATOR "|"
    CMAKE_GENERATOR "Ninja Multi-Config" CMAKE_ARGS ${_args}
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config $<CONFIG> --target "ggml-${_backend}" --parallel
    BUILD_BYPRODUCTS "${_module}"
    INSTALL_COMMAND "" UPDATE_COMMAND "" TEST_COMMAND ""
    EXCLUDE_FROM_ALL TRUE)
  add_library(qctsm-gpu-backend MODULE IMPORTED GLOBAL)
  set_target_properties(qctsm-gpu-backend PROPERTIES
    IMPORTED_CONFIGURATIONS "Debug;Release;RelWithDebInfo;MinSizeRel"
    IMPORTED_LOCATION "${_build}/bin/Release/ggml-${_backend}.dll")
  foreach(_config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${_config}" _config_upper)
    set_property(TARGET qctsm-gpu-backend PROPERTY IMPORTED_LOCATION_${_config_upper}
      "${_build}/bin/${_config}/ggml-${_backend}.dll")
  endforeach()
  add_dependencies(qctsm-gpu-backend qctsm-gpu-backend-build)
  add_custom_target(qctsm-gpu-backend-abi
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/qctsm-gpu-abi.py"
      --module "${_module}" --base "$<TARGET_FILE:ggml-base>"
      --report "${CMAKE_BINARY_DIR}/gpu-abi-$<CONFIG>.json"
    DEPENDS qctsm-gpu-backend-build ggml-base VERBATIM)
  set(${output} qctsm-gpu-backend PARENT_SCOPE)
endfunction()

function(qctsm_install_gpu_dependencies module_target)
  if(QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_BACKEND MATCHES "^(cpu|metal)$")
    return()
  endif()
  set(QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_MANIFEST "" CACHE FILEPATH "GPU SDK manifest produced by qctsm-gpu-sdk.py")
  if(NOT EXISTS "${QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_MANIFEST}")
    message(FATAL_ERROR "GPU installation requires QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_MANIFEST from tools/qctsm-gpu-sdk.py")
  endif()
  set(_script "${CMAKE_BINARY_DIR}/qctsm-install-gpu-$<CONFIG>.cmake")
  if(WIN32)
    set(_module_directory "${CMAKE_INSTALL_BINDIR}")
  else()
    set(_module_directory "${CMAKE_INSTALL_LIBDIR}")
  endif()
  file(GENERATE OUTPUT "${_script}" CONTENT
"execute_process(COMMAND \"${Python3_EXECUTABLE}\" \"${PROJECT_SOURCE_DIR}/tools/qctsm-gpu-runtime.py\"
  --variant \"${QCOPILOTS_TOOL_SEMANTIC_MATCHING_VARIANT}\" --sdk-manifest \"${QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_MANIFEST}\"
  --backend-library \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${_module_directory}/$<TARGET_FILE_NAME:${module_target}>\" --stage-root \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\"
  --report \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/share/qctsm/gpu-runtime.json\"
  RESULT_VARIABLE _gpu_result)
if(NOT _gpu_result EQUAL 0)
  message(FATAL_ERROR \"GPU runtime deployment failed: \${_gpu_result}\")
endif()
")
  install(SCRIPT "${_script}")
endfunction()
