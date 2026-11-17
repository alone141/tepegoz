add_library(service_registry STATIC ServiceRegistry.cpp)
target_include_directories(service_registry PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
# Public: the header holds config_section::ConfigValue. Private: the extension of a registered file.
target_link_libraries(service_registry PUBLIC config_section PRIVATE config_source)
