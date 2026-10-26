#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace journal_server
{

constexpr std::string_view default_endpoint = "tcp://*:6670";
constexpr std::string_view default_journalctl = "journalctl";
constexpr std::string_view default_systemctl = "systemctl";
constexpr int default_lines = 50;
constexpr int max_lines = 1000;
constexpr std::size_t max_name_length = 63;
constexpr std::size_t max_cursor_length = 512;
// More journal output than this is cut at its last whole line.
constexpr std::size_t max_output_bytes = 4 * 1024 * 1024;
constexpr std::chrono::milliseconds command_timeout{5000};
constexpr std::chrono::milliseconds poll_period{200};
// The version a "version" request answers with; it grows when the protocol does.
constexpr int protocol_version = 1;

enum class TargetKind
{
    Manager,
    Service,
    Pid,
};

// One journal read: whose lines, how many, and from where.
struct JournalRequest
{
    TargetKind kind = TargetKind::Manager;
    // The service's ProcessName, or the pid in decimal; empty for the manager.
    std::string value;
    int lines = default_lines;
    // Lines after this journal cursor; empty for the newest lines.
    std::string afterCursor;
};

// Where the manager's own journal entries, and those of its services, are found.
struct ManagerIdentity
{
    // The manager's systemd unit, such as processmanager.service; empty when it runs outside one.
    std::string unit;
    // Matched instead of the unit when there is none.
    int pid = 0;
    // journald's _SYSTEMD_CGROUP of the folder that holds task_pm and every task_<name>, such as
    // /system.slice/processmanager.service; "/" outside a unit.
    std::string serviceCgroupParent = "/";
};

enum class RequestCode
{
    Ok,
    UnknownRequest,
    UnknownKind,
    BadService,
    BadPid,
    BadLines,
    BadCursor,
    NoUnit,
};

enum class StartCode
{
    Ok,
    BindFailed,
};

/**
 * @brief Work out the manager's identity from the cgroup that holds its services' cgroups.
 * @param serviceCgroupParent journald form of that cgroup, such as /system.slice/processmanager.service.
 * @param pid The manager's pid.
 * @return The identity. Its unit is the innermost path component that ends in .service, or empty without one.
 */
ManagerIdentity IdentityFromCgroup(std::string_view serviceCgroupParent, int pid);

/**
 * @brief Read the frames of a "journal" request: kind, value, lines and cursor, after the request name.
 * @param frames The request's frames, starting with "journal".
 * @param out Receives the request on success.
 * @return RequestCode::Ok, or what is wrong with the request.
 */
RequestCode ParseJournalRequest(const std::vector<std::string>& frames, JournalRequest& out);

/**
 * @brief Build the journalctl arguments, without the program name, that read @p request.
 * @param request The read, as ParseJournalRequest() returns it.
 * @param manager Where the manager's and its services' entries are.
 * @param out Receives the arguments on success.
 * @return RequestCode::Ok, or what is wrong with the request.
 */
RequestCode BuildJournalArgs(const JournalRequest& request, const ManagerIdentity& manager, std::vector<std::string>& out);

/**
 * @brief Build the systemctl arguments, without the program name, that show the manager's unit status.
 * @param manager The manager's identity.
 * @param out Receives the arguments on success.
 * @return RequestCode::Ok, or NoUnit when the manager runs outside a systemd unit.
 */
RequestCode BuildUnitStatusArgs(const ManagerIdentity& manager, std::vector<std::string>& out);

/**
 * @brief Return the text a client sees for a request that was refused.
 * @param code A failure of ParseJournalRequest(), BuildJournalArgs() or BuildUnitStatusArgs().
 * @return A short English explanation.
 */
std::string Describe(RequestCode code);

class JournalServer
{
public:
    /**
     * @brief Keep what the server needs. Start() binds the endpoint and starts answering.
     * @param endpoint ZeroMQ endpoint to bind, normally default_endpoint.
     * @param manager Where the manager's and its services' journal entries are.
     * @param journalctl journalctl to run: a path, or a name looked up on PATH.
     * @param systemctl systemctl to run: a path, or a name looked up on PATH.
     */
    JournalServer(std::string endpoint, ManagerIdentity manager, std::string journalctl, std::string systemctl);

    /**
     * @brief Stop answering. A journalctl still running is killed.
     */
    ~JournalServer();

    JournalServer(const JournalServer&) = delete;
    JournalServer& operator=(const JournalServer&) = delete;

    /**
     * @brief Take over the serving thread of @p other.
     * @param other Server to move from. It no longer serves.
     */
    JournalServer(JournalServer&& other) noexcept;

    /**
     * @brief Stop this server and take over the serving thread of @p other.
     * @param other Server to move from. It no longer serves.
     * @return This server.
     */
    JournalServer& operator=(JournalServer&& other) noexcept;

    /**
     * @brief Bind the endpoint and answer requests on a thread of its own.
     * @param error Receives ZeroMQ's message when the bind fails.
     * @return StartCode::Ok, or BindFailed. A server that already serves returns Ok.
     */
    StartCode Start(std::string& error);

    /**
     * @brief Return the endpoint the server is bound to, with a wildcard port resolved.
     * @return The bound endpoint, or empty before a successful Start().
     */
    std::string BoundEndpoint() const;

private:
    struct Shared;

    static void Serve(std::shared_ptr<Shared> shared);
    void Stop();

    std::shared_ptr<Shared> shared;
    std::thread worker;
};

} // namespace journal_server
