add_library(config_section STATIC ConfigSection.cpp)
target_include_directories(config_section PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
# fkYAML stays private: only ConfigSection.cpp includes its header.
target_link_libraries(config_section PRIVATE fkYAML::fkYAML)
