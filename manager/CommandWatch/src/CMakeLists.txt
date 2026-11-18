add_library(command_watch STATIC CommandWatch.cpp)
target_include_directories(command_watch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
# The header only names the report types; the source reads them.
target_link_libraries(command_watch PRIVATE status_table)
