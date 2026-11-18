#pragma once
#include "StdioMode.hpp"
#include <filesystem>
namespace config_section
{

class ConfigSection;

} // namespace config_section
namespace tepegoz
{

struct StdioPolicy
{
    void UpdateArgs(config_section::ConfigSection& section);


    StdioMode stdioMode = StdioMode::Inherit;
    std::filesystem::path stdioFilePath{"/tmp/output.txt"};
    bool stdioAppend = false;
};

} // namespace tepegoz
