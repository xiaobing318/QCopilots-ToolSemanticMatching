find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

if(TARGET qctsm-service AND NOT QCOPILOTS_TOOL_SEMANTIC_MATCHING_CORE_ONLY)
  add_test(NAME qctsm-quality-real-model
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/run_service_quality.py"
      --service "$<TARGET_FILE:qctsm-service>"
      --cli "$<TARGET_FILE:qctsm-cli>"
      --output-dir "${QCOPILOTS_TOOL_SEMANTIC_MATCHING_TEST_ROOT}/quality")
  set_tests_properties(qctsm-quality-real-model PROPERTIES
    LABELS "quality;real-model;integration"
    TIMEOUT 1800
    ENVIRONMENT "QCOPILOTS_TOOL_SEMANTIC_MATCHING_MODEL_DIR=${QCOPILOTS_TOOL_SEMANTIC_MATCHING_MODEL_DIR}")
endif()
