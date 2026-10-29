add_library(qctsm-service-library STATIC
  service_config.cpp service_config.h
  worker_broker.cpp worker_broker.h
  task_service.cpp task_service.h
  http_server.cpp http_server.h)
target_compile_features(qctsm-service-library PUBLIC cxx_std_20)
target_include_directories(qctsm-service-library PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(qctsm-service-library PUBLIC Qt6::Core Qt6::Network qctsm::retrieval qctsm::http_provider qctsm-model-files PRIVATE httplib::httplib)
add_executable(qctsm-service main.cpp)
target_link_libraries(qctsm-service PRIVATE qctsm-service-library)
