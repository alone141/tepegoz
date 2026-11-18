include(GoogleTest)
add_executable(logger_tests LoggerTest.cpp)
target_link_libraries(logger_tests PRIVATE logger GTest::gtest_main)
gtest_discover_tests(logger_tests)
