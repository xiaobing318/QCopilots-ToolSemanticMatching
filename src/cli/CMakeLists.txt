add_executable(qctsm-cli main.cpp)
target_compile_features(qctsm-cli PRIVATE cxx_std_20)
target_compile_definitions(qctsm-cli PRIVATE QCTSM_APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(qctsm-cli PRIVATE Qt6::Core Qt6::Network qctsm-service-library)

if(QCOPILOTS_TOOL_SEMANTIC_MATCHING_BUILD_TESTS)
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  add_test(NAME qctsm-cli-protocol
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/protocol_test.py"
            --cli "$<TARGET_FILE:qctsm-cli>" --qt-bin "$<TARGET_FILE_DIR:Qt6::Core>")
  set_tests_properties(qctsm-cli-protocol PROPERTIES LABELS "unit;protocol" TIMEOUT 60)
endif()

