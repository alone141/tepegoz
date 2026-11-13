#pragma once
#include "StdioMode.hpp"
#include <filesystem>
namespace config_section
{
    class ConfigSection;
}
namespace process_manager
{
    struct StdioPolicyStruct
    {
        void UpdateArgs(config_section::ConfigSection& section);


        StdioModeEnum StdioMode = StdioModeEnum::Inherit;
        std::filesystem::path StdioFilePath{};
        bool StdioAppend = true;
    };
}
