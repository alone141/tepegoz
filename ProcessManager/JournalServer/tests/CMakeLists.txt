find_package(Threads REQUIRED)

include(GoogleTest)
add_executable(journal_server_tests JournalServerTest.cpp)
# The tests ask the server through a ZeroMQ socket of their own, and run it against stand-in journalctl scripts.
target_link_libraries(journal_server_tests PRIVATE journal_server cppzmq-static Threads::Threads GTest::gtest_main)
gtest_discover_tests(journal_server_tests)
