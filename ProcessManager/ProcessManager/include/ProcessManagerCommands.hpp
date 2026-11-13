#pragma once
#include <cstdint>
#include <string_view>
#include <string>

namespace process_manager
{
    enum class CommandCode : char
    {
        Unknown = -1,
        StartServiceWithArgs = 77,
        StartService,
        StopService,
        DisableService,
        RestartService,
        GetVersionInfo
    };
    struct CommandMessage
    {
        CommandCode command;
        char serviceName[32];
        char args[32];
    };

    struct __attribute__((packed)) VersionInfo
    {
        uint8_t major = 0;
        uint8_t minor = 0;
        uint16_t patch = 0;
    };
    // The answer to GetVersionInfo: the manager's own version, from the CMake project.
    struct __attribute__((packed)) VersionReport
    {
        CommandCode command; // 1 byte
        VersionInfo manager;
        uint8_t reserved[3];
    };
    namespace comm
    {
        constexpr std::string_view command_server_id = "PMC";
        constexpr std::string_view gui_id = "GUI";
        inline const std::string process_manager_id = "BPM"; // std::string so that it can be logged
        inline const char* command_server_port = "tcp://localhost:5557";
        inline const char* health_publish_port_detailed = "tcp://*:6667";
        inline const char* health_listen_port_detailed = "tcp://localhost:6667";
        // Journal reads for a monitor GUI on another machine (JournalServer).
        inline const char* journal_server_port = "tcp://*:6670";
    }
}