#include <StdioConfigStruct.h>
#include "ConfigSection.hpp"
#include <string>
using namespace process_manager;
void process_manager::StdioPolicyStruct::UpdateArgs(config_section::ConfigSection& section)
{
    // The choices in StdioModeEnum order.
    StdioMode = static_cast<StdioModeEnum>(section.ReadChoice("StdioMode", { "Inherit", "Pipe", "File", "NullDevice" }, 0));
    StdioFilePath = std::filesystem::path(section.ReadString("StdioFilePath", "/tmp/output.txt")); //todo isme gore
    StdioAppend = section.ReadBool("StdioAppend", false);
}
