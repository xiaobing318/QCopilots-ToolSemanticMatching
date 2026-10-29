# SPDX-License-Identifier: MIT
add_library(qctsm-http-provider STATIC http_embedding_provider.cpp http_embedding_provider.h)
add_library(qctsm::http_provider ALIAS qctsm-http-provider)
target_compile_features(qctsm-http-provider PUBLIC cxx_std_20)
target_include_directories(qctsm-http-provider PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(qctsm-http-provider PUBLIC qctsm-retrieval Qt6::Network)
set_target_properties(qctsm-http-provider PROPERTIES POSITION_INDEPENDENT_CODE ON)
