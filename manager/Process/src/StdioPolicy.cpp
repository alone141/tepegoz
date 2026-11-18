#include "StdioPolicy.hpp"
#include "ConfigSection.hpp"
#include <string>

namespace tepegoz
{

void StdioPolicy::UpdateArgs(config_section::ConfigSection& section)
{
    // The choices in StdioMode order.
    stdioMode = static_cast<StdioMode>(section.ReadChoice("StdioMode", { "Inherit", "Pipe", "File", "NullDevice" }, 0));
    stdioFilePath = std::filesystem::path(section.ReadString("StdioFilePath", "/tmp/output.txt")); // todo: by name
    stdioAppend = section.ReadBool("StdioAppend", false);
}

} // namespace tepegoz
