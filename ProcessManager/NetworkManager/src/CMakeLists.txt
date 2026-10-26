find_package(Threads REQUIRED)

add_library(network_manager STATIC NetworkManager.cpp)
target_include_directories(network_manager PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
# cppzmq-static (cppzmq with a static libzmq) is vendored and added in the root CMakeLists.txt. The header names
# no ZeroMQ type, so it stays private.
target_link_libraries(network_manager PRIVATE cppzmq-static logger Threads::Threads)
