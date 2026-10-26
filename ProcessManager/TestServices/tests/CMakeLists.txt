find_package(Threads REQUIRED)

include(GoogleTest)
add_executable(test_service_tests TestServiceTest.cpp)
# The tests take heartbeats on a ZeroMQ router of their own and build cxxopts options.
target_link_libraries(test_service_tests PRIVATE test_service cppzmq-static cxxopts::cxxopts Threads::Threads GTest::gtest_main)
gtest_discover_tests(test_service_tests)
