add_library(status_table STATIC StatusTable.cpp)
target_include_directories(status_table PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
