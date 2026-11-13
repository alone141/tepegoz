#pragma once
namespace process_manager
{
enum class StdioMode
{
    Inherit,
    Pipe,
    File,
    NullDevice
};
}
