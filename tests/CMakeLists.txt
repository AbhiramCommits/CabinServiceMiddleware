include(GoogleTest)

add_executable(test_log test_log.cpp)
target_link_libraries(test_log PRIVATE cabin_core GTest::gtest_main)
cabin_set_warnings(test_log)
gtest_discover_tests(test_log)

add_executable(test_trace test_trace.cpp)
target_link_libraries(test_trace PRIVATE cabin_core GTest::gtest_main)
cabin_set_warnings(test_trace)
gtest_discover_tests(test_trace)

add_executable(test_serdes test_serdes.cpp)
target_link_libraries(test_serdes PRIVATE cabin_core GTest::gtest_main)
cabin_set_warnings(test_serdes)
gtest_discover_tests(test_serdes)

add_executable(test_frame test_frame.cpp)
target_link_libraries(test_frame PRIVATE cabin_core GTest::gtest_main)
cabin_set_warnings(test_frame)
gtest_discover_tests(test_frame)

add_executable(test_broker test_broker.cpp)
target_link_libraries(test_broker PRIVATE cabin_ipc GTest::gtest_main)
cabin_set_warnings(test_broker)
gtest_discover_tests(test_broker)

# Used by scripts/test.sh to prove a real log line parses as JSON with the
# standard-library Python json module (no C++ JSON parser needed at step 1).
add_executable(cabin_log_probe log_probe.cpp)
target_link_libraries(cabin_log_probe PRIVATE cabin_core)
cabin_set_warnings(cabin_log_probe)
