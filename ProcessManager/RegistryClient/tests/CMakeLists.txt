find_package(Threads REQUIRED)

include(GoogleTest)
add_executable(registry_client_tests RegistryClientTest.cpp)
# The tests read values and requests with config_section, write the manager's answers with service_registry, and
# answer from a stand-in registry: a ZeroMQ ROUTER of their own.
target_link_libraries(registry_client_tests
    PRIVATE registry_client config_section service_registry cppzmq-static Threads::Threads GTest::gtest_main)
gtest_discover_tests(registry_client_tests)
