add_library(test_service STATIC TestService.cpp)
target_include_directories(test_service PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
# The header names no ZeroMQ or cxxopts type, so both stay private.
target_link_libraries(test_service PRIVATE cppzmq-static cxxopts::cxxopts)
