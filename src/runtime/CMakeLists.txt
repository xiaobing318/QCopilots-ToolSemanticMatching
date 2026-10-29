add_library(qctsm-model-files STATIC model_files.cpp model_files.h)
target_compile_features(qctsm-model-files PUBLIC cxx_std_20)
target_include_directories(qctsm-model-files PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(qctsm-model-files PUBLIC Qt6::Core Qt6::Network)
qt_add_resources(qctsm-model-files qctsm_models
  PREFIX "/qctsm" BASE "${PROJECT_SOURCE_DIR}/config"
  FILES "${PROJECT_SOURCE_DIR}/config/qctsm-models.lock.json")

add_library(qctsm-runtime STATIC
  llama_embedding_provider.cpp llama_embedding_provider.h
  BackendSelection.cpp BackendSelection.h BackendLoader.cpp BackendLoader.h
  BackendPaths.h utf8_locale.h)
target_compile_features(qctsm-runtime PUBLIC cxx_std_20)
target_include_directories(qctsm-runtime PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(qctsm-runtime PUBLIC qctsm-retrieval qctsm-model-files Qt6::Core PRIVATE llama)
if(WIN32)
  target_compile_definitions(qctsm-runtime PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
