add_library(status_client STATIC StatusClient.cpp)
target_include_directories(status_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
# The header names no ZeroMQ or report type. The report and command layouts are header-only structs of Process and
# ProcessManager; their libraries are not needed.
target_include_directories(status_client
    PRIVATE
        $<TARGET_PROPERTY:Process,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:ProcessManager,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(status_client PRIVATE command_watch status_table cppzmq-static)
