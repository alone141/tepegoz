find_package(Threads REQUIRED)

include(GoogleTest)
add_executable(status_client_tests StatusClientTest.cpp)
# The tests run a stand-in manager: a report publisher and a broker on ZeroMQ sockets of their own.
target_include_directories(status_client_tests
    PRIVATE
        $<TARGET_PROPERTY:Process,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:ProcessManager,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(status_client_tests
    PRIVATE status_client command_watch status_table cppzmq-static Threads::Threads GTest::gtest_main)
gtest_discover_tests(status_client_tests)
