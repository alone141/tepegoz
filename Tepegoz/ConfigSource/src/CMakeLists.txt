add_library(config_source STATIC ConfigSource.cpp)
target_include_directories(config_source PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
# Public: the header holds config_section::ConfigValue.
target_link_libraries(config_source PUBLIC config_section)
