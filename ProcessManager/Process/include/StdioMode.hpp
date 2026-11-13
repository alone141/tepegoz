#pragma once
namespace process_manager
{
    enum class StdioModeEnum
    {
        Inherit,
        Pipe,
        File,
        NullDevice
    };
}
