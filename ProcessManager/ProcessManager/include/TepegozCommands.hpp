#pragma once
#include <cstdint>
#include <string_view>
#include <string>

namespace tepegoz
{

enum class CommandCode : char
{
    Unknown = -1,
    StartServiceWithArgs = 77,
    StartService,
    StopService,
    DisableService,
    RestartService,
    GetVersionInfo,
    ReloadConfig // reads the configuration again and applies what changed; answered with a ReloadReport
};
struct CommandMessage
{
    CommandCode command;
    char serviceName[32];
    char args[32];
};
// ReloadConfig with these args is not answered: the sender does not wait (--no-wait).
constexpr std::string_view reload_without_reply = "no-reply";

// Wire values: the command line and the GUI read them.
enum class ReloadResult : uint8_t
{
    Applied,
    NoSource,            // the configuration was not read from a file
    MainFileNotOpened,
    MainFileSyntaxError,
    NoServicesList,      // the main file's top level is not a mapping, or its Services is not a list
    FolderNotReadable,   // services.d exists but could not be listed
    DependencyCycle,
    DuplicateName,       // two services of the new configuration have the same ProcessName
};
// The answer to ReloadConfig, sent to whoever asked. The counts are those of the reload: services added, removed,
// changed (their new definition waits for their next start), kept because their new definition could not be read,
// skipped (new, but their definition could not be read) and unchanged. All are 0 unless the result is Applied.
struct __attribute__((packed)) ReloadReport
{
    CommandCode command; // ReloadConfig
    ReloadResult result;
    uint16_t added;
    uint16_t removed;
    uint16_t changed;
    uint16_t kept;
    uint16_t skipped;
    uint16_t unchanged;
    uint8_t reserved[2];
};
static_assert(sizeof(ReloadReport) == 16, "the command line and the GUI read 16-byte reload reports");

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
inline const std::string tepegoz_id = "BPM"; // std::string so that it can be logged
inline const char* command_server_port = "tcp://localhost:5557";
inline const char* health_publish_port_detailed = "tcp://*:6667";
inline const char* health_listen_port_detailed = "tcp://localhost:6667";
// Journal reads for a monitor GUI on another machine (JournalServer).
inline const char* journal_server_port = "tcp://*:6670";
// Registering services (ServiceRegistry): this machine only, or every interface with RemoteRegistration: true.
inline const char* registry_port_local = "tcp://127.0.0.1:6671";
inline const char* registry_port_remote = "tcp://*:6671";
inline const char* registry_connect = "tcp://localhost:6671";

} // namespace comm

} // namespace tepegoz
