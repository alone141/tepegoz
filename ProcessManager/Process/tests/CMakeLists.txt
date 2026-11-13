include(GoogleTest)
add_executable(process_tests ProcessTest.cpp)
# The tests start the TestServices programs as services, so they name the folder those programs are built in.
target_compile_definitions(process_tests PRIVATE PMT_BIN_DIR="$<TARGET_FILE_DIR:pmt_steady>")
add_dependencies(process_tests pmt_steady pmt_exit pmt_crash pmt_stubborn pmt_forker pmt_env)
target_link_libraries(process_tests PRIVATE Process GTest::gtest_main)
gtest_discover_tests(process_tests)
