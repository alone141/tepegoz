include(GoogleTest)
add_executable(process_tests ProcessTest.cpp)
# The tests start the TestServices programs as services, so they name the folder those programs are built in.
target_compile_definitions(process_tests PRIVATE TPZ_BIN_DIR="$<TARGET_FILE_DIR:tpz_steady>")
add_dependencies(process_tests tpz_steady tpz_exit tpz_crash tpz_stubborn tpz_forker tpz_env)
target_link_libraries(process_tests PRIVATE Process GTest::gtest_main)
gtest_discover_tests(process_tests)
