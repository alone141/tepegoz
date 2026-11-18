find_package(Threads REQUIRED)

add_library(journal_server STATIC JournalServer.cpp)
target_include_directories(journal_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
# The header names no ZeroMQ type, so cppzmq-static stays private.
target_link_libraries(journal_server PRIVATE cppzmq-static Threads::Threads)
