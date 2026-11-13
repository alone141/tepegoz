#pragma once
#include "StdioMode.hpp"
#include <filesystem>
namespace config_section
{

class ConfigSection;

} // namespace config_section
namespace process_manager
{

struct StdioPolicy
{
    void UpdateArgs(config_section::ConfigSection& section);


    StdioMode stdioMode = StdioMode::Inherit;
    std::filesystem::path stdioFilePath{};
    bool stdioAppend = true;
};

} // namespace process_manager
