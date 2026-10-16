find_package(Threads REQUIRED)

add_library(logger STATIC Logger.cpp)
target_include_directories(logger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(logger PRIVATE Threads::Threads)
