add_library(registry_client STATIC RegistryClient.cpp)
target_include_directories(registry_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
# The header only names config_section::ConfigValue. Private: reading values and answers, writing requests as JSON,
# the restart mark's note of the status table, and the socket.
target_link_libraries(registry_client PRIVATE config_section service_registry status_table cppzmq-static)
