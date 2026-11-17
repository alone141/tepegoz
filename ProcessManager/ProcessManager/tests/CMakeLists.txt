include(GoogleTest)
add_executable(process_manager_tests ProcessManagerTest.cpp)
# The tests build services from configurations; none is started, so no socket is opened.
target_link_libraries(process_manager_tests PRIVATE ProcessManager config_source GTest::gtest_main)
gtest_discover_tests(process_manager_tests)
