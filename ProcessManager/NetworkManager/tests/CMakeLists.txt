find_package(Threads REQUIRED)

include(GoogleTest)
add_executable(network_manager_tests NetworkManagerTest.cpp)
# The tests talk to the broker through their own ZeroMQ dealers.
target_link_libraries(network_manager_tests PRIVATE network_manager logger cppzmq-static Threads::Threads GTest::gtest_main)
gtest_discover_tests(network_manager_tests)
