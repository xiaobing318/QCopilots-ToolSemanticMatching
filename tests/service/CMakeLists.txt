add_executable(qctsm-service-test service_test.cpp)
target_link_libraries(qctsm-service-test PRIVATE qctsm-service-library Qt6::Test httplib::httplib)
add_test(NAME qctsm-service COMMAND qctsm-service-test)
set_tests_properties(qctsm-service PROPERTIES TIMEOUT 60)
