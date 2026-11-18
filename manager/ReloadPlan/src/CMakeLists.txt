add_library(reload_plan STATIC ReloadPlan.cpp)
target_include_directories(reload_plan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
# Public: the header holds config_section::ConfigValue. The source reads config_source::LoadedConfig.
target_link_libraries(reload_plan PUBLIC config_section PRIVATE config_source)
