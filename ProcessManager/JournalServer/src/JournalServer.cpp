#include "JournalServer.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#include <zmq.hpp>
#include <zmq_addon.hpp>

extern char** environ;

namespace journal_server
{

namespace
{

constexpr std::chrono::milliseconds read_poll_period{100};
constexpr std::size_t max_error_bytes = 64 * 1024;
constexpr int max_pid_digits = 9;
constexpr int max_lines_digits = 9;

enum class RunCode
{
    Ok,
    SpawnFailed,
    TimedOut,
    Cancelled,
};

struct CommandResult
{
    int exitCode = -1;
    std::string output;
    std::string errors;
    bool truncated = false;
};

bool IsDigits(std::string_view text, std::size_t maxDigits)
{
    if (text.empty() || text.size() > maxDigits)
    {
        return false;
    }

    for (const char c : text)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
    }

    return true;
}

bool IsServiceName(std::string_view name)
{
    if (name.empty() || name.size() > max_name_length || name == "." || name == "..")
    {
        return false;
    }

    for (const char c : name)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7f || c == '/')
        {
            return false;
        }
    }

    return true;
}

// A journal cursor is printable ASCII without spaces, such as "s=…;i=…;b=…;m=…;t=…;x=…".
bool IsCursor(std::string_view cursor)
{
    if (cursor.size() > max_cursor_length)
    {
        return false;
    }

    for (const char c : cursor)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte >= 0x7f)
        {
            return false;
        }
    }

    return true;
}

// The path with one leading slash and no empty components: "system.slice//a.service/" gives "/system.slice/a.service".
std::string NormalCgroup(std::string_view path)
{
    std::string normal{};
    std::size_t start = 0;
    while (start <= path.size())
    {
        const std::size_t slash = path.find('/', start);
        const std::string_view part = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (!part.empty())
        {
            normal += '/';
            normal += part;
        }

        if (slash == std::string_view::npos)
        {
            break;
        }

        start = slash + 1;
    }

    return normal.empty() ? "/" : normal;
}

std::string JoinCgroup(std::string_view parent, std::string_view child)
{
    std::string path = NormalCgroup(parent);
    if (path != "/")
    {
        path += '/';
    }

    path += child;
    return path;
}

void ReadInto(int fd, std::string& into, std::size_t limit, bool& open, bool& full)
{
    char buffer[16384];
    const ssize_t count = ::read(fd, buffer, sizeof(buffer));
    if (count == 0)
    {
        open = false;
        return;
    }

    if (count < 0)
    {
        if (errno != EINTR && errno != EAGAIN)
        {
            open = false;
        }

        return;
    }

    const std::size_t room = into.size() < limit ? limit - into.size() : 0;
    into.append(buffer, std::min(room, static_cast<std::size_t>(count)));
    full = static_cast<std::size_t>(count) > room;
}

// Runs program with args and no shell, collecting stdout and stderr. A run that outlasts command_timeout, or is
// cancelled through stop, is killed. Output beyond max_output_bytes ends the run and is cut at its last whole line.
RunCode RunCommand(const std::string& program, const std::vector<std::string>& args, const std::atomic<bool>& stop,
                   CommandResult& out)
{
    int outPipe[2]{-1, -1};
    int errPipe[2]{-1, -1};
    if (::pipe2(outPipe, O_CLOEXEC) != 0)
    {
        out.errors = std::strerror(errno);
        return RunCode::SpawnFailed;
    }

    if (::pipe2(errPipe, O_CLOEXEC) != 0)
    {
        out.errors = std::strerror(errno);
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        return RunCode::SpawnFailed;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);

    // The manager blocks SIGINT and SIGTERM in every thread; the command starts with nothing blocked.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t blocked;
    sigemptyset(&blocked);
    posix_spawnattr_setsigmask(&attributes, &blocked);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    std::vector<char*> argv{};
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& arg : args)
    {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    argv.push_back(nullptr);
    pid_t pid = -1;
    const int spawned = ::posix_spawnp(&pid, program.c_str(), &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    ::close(outPipe[1]);
    ::close(errPipe[1]);
    if (spawned != 0)
    {
        out.errors = program + ": " + std::strerror(spawned);
        ::close(outPipe[0]);
        ::close(errPipe[0]);
        return RunCode::SpawnFailed;
    }

    RunCode code = RunCode::Ok;
    bool outOpen = true;
    bool errOpen = true;
    const auto deadline = std::chrono::steady_clock::now() + command_timeout;
    while (outOpen || errOpen)
    {
        if (stop)
        {
            code = RunCode::Cancelled;
            break;
        }

        if (std::chrono::steady_clock::now() > deadline)
        {
            code = RunCode::TimedOut;
            break;
        }

        pollfd fds[2]{{outOpen ? outPipe[0] : -1, POLLIN, 0}, {errOpen ? errPipe[0] : -1, POLLIN, 0}};
        const int ready = ::poll(fds, 2, static_cast<int>(read_poll_period.count()));
        if (ready <= 0)
        {
            continue;
        }

        bool full = false;
        if (fds[0].revents != 0)
        {
            ReadInto(outPipe[0], out.output, max_output_bytes, outOpen, full);
        }

        if (full)
        {
            out.truncated = true;
            break;
        }

        if (fds[1].revents != 0)
        {
            bool errorsFull = false;
            ReadInto(errPipe[0], out.errors, max_error_bytes, errOpen, errorsFull);
        }
    }

    if (code != RunCode::Ok || out.truncated)
    {
        ::kill(pid, SIGKILL);
    }

    ::close(outPipe[0]);
    ::close(errPipe[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
    {
    }

    out.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (out.truncated)
    {
        const std::size_t lastLine = out.output.rfind('\n');
        out.output.resize(lastLine == std::string::npos ? 0 : lastLine + 1);
    }

    return code;
}

std::string Trimmed(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
    {
        text.pop_back();
    }

    return text;
}

std::string RunFailure(const std::string& program, RunCode code, const CommandResult& result)
{
    if (code == RunCode::SpawnFailed)
    {
        return "cannot run " + result.errors;
    }

    if (code == RunCode::TimedOut)
    {
        return program + " took longer than " + std::to_string(command_timeout.count()) + " ms";
    }

    if (code == RunCode::Cancelled)
    {
        return "the journal server is stopping";
    }

    const std::string errors = Trimmed(result.errors);
    return errors.empty() ? program + " failed (exit " + std::to_string(result.exitCode) + ")" : errors;
}

} // namespace

ManagerIdentity IdentityFromCgroup(std::string_view serviceCgroupParent, int pid)
{
    ManagerIdentity identity{};
    identity.pid = pid;
    identity.serviceCgroupParent = NormalCgroup(serviceCgroupParent);
    std::string_view rest{identity.serviceCgroupParent};
    while (!rest.empty())
    {
        const std::size_t slash = rest.rfind('/');
        const std::string_view component = rest.substr(slash == std::string_view::npos ? 0 : slash + 1);
        constexpr std::string_view suffix = ".service";
        if (component.size() > suffix.size() && component.substr(component.size() - suffix.size()) == suffix)
        {
            identity.unit = std::string{component};
            break;
        }

        rest = rest.substr(0, slash == std::string_view::npos ? 0 : slash);
    }

    return identity;
}

RequestCode ParseJournalRequest(const std::vector<std::string>& frames, JournalRequest& out)
{
    if (frames.empty() || frames[0] != "journal")
    {
        return RequestCode::UnknownRequest;
    }

    JournalRequest request{};
    const std::string kind = frames.size() > 1 ? frames[1] : "";
    if (kind == "manager")
    {
        request.kind = TargetKind::Manager;
    }
    else if (kind == "service")
    {
        request.kind = TargetKind::Service;
    }
    else if (kind == "pid")
    {
        request.kind = TargetKind::Pid;
    }
    else
    {
        return RequestCode::UnknownKind;
    }

    request.value = frames.size() > 2 ? frames[2] : "";
    const std::string lines = frames.size() > 3 ? frames[3] : "";
    if (!lines.empty())
    {
        if (!IsDigits(lines, max_lines_digits))
        {
            return RequestCode::BadLines;
        }

        std::from_chars(lines.data(), lines.data() + lines.size(), request.lines);
    }

    request.afterCursor = frames.size() > 4 ? frames[4] : "";
    out = std::move(request);
    return RequestCode::Ok;
}

RequestCode BuildJournalArgs(const JournalRequest& request, const ManagerIdentity& manager, std::vector<std::string>& out)
{
    std::vector<std::string> args{};
    if (request.kind == TargetKind::Manager)
    {
        if (!manager.unit.empty())
        {
            args = {"-u", manager.unit};
        }
        else
        {
            args = {"_PID=" + std::to_string(manager.pid)};
        }
    }
    else if (request.kind == TargetKind::Service)
    {
        if (!IsServiceName(request.value))
        {
            return RequestCode::BadService;
        }

        args = {"_SYSTEMD_CGROUP=" + JoinCgroup(manager.serviceCgroupParent, "task_" + request.value)};
    }
    else
    {
        int pid = 0;
        if (!IsDigits(request.value, max_pid_digits) ||
            std::from_chars(request.value.data(), request.value.data() + request.value.size(), pid).ec != std::errc{} ||
            pid <= 0)
        {
            return RequestCode::BadPid;
        }

        args = {"_PID=" + std::to_string(pid)};
    }

    if (!IsCursor(request.afterCursor))
    {
        return RequestCode::BadCursor;
    }

    const int lines = std::clamp(request.lines, 1, max_lines);
    args.insert(args.end(), {"-n", std::to_string(lines), "--no-pager", "--all", "--quiet", "-o", "json"});
    if (!request.afterCursor.empty())
    {
        args.push_back("--after-cursor=" + request.afterCursor);
    }

    out = std::move(args);
    return RequestCode::Ok;
}

RequestCode BuildUnitStatusArgs(const ManagerIdentity& manager, std::vector<std::string>& out)
{
    if (manager.unit.empty())
    {
        return RequestCode::NoUnit;
    }

    out = {"status", manager.unit, "--no-pager", "-l", "-n", "0"};
    return RequestCode::Ok;
}

std::string Describe(RequestCode code)
{
    switch (code)
    {
    case RequestCode::Ok:
        return "ok";
    case RequestCode::UnknownRequest:
        return "unknown request; the journal server answers version, journal and unit-status";
    case RequestCode::UnknownKind:
        return "unknown journal target; use manager, service or pid";
    case RequestCode::BadService:
        return "bad service name";
    case RequestCode::BadPid:
        return "bad pid";
    case RequestCode::BadLines:
        return "bad line count";
    case RequestCode::BadCursor:
        return "bad journal cursor";
    case RequestCode::NoUnit:
        return "the manager runs outside a systemd unit";
    }

    return "unknown error";
}

namespace
{

struct ServerSettings
{
    std::string endpoint;
    ManagerIdentity manager;
    std::string journalctl;
    std::string systemctl;
};

// Answers one request: the reply's status ("ok" or "error") and its payload.
void Handle(const ServerSettings& settings, const std::atomic<bool>& stop, const std::vector<std::string>& request,
            std::string& status, std::string& payload)
{
    status = "error";
    const std::string name = request.empty() ? "" : request[0];
    if (name == "version")
    {
        status = "ok";
        payload = std::to_string(protocol_version);
        return;
    }

    std::vector<std::string> args{};
    RequestCode code = RequestCode::UnknownRequest;
    const bool journal = name == "journal";
    if (journal)
    {
        JournalRequest parsed{};
        code = ParseJournalRequest(request, parsed);
        if (code == RequestCode::Ok)
        {
            code = BuildJournalArgs(parsed, settings.manager, args);
        }
    }
    else if (name == "unit-status")
    {
        code = BuildUnitStatusArgs(settings.manager, args);
    }

    if (code != RequestCode::Ok)
    {
        payload = Describe(code);
        return;
    }

    const std::string& program = journal ? settings.journalctl : settings.systemctl;
    CommandResult result{};
    const RunCode run = RunCommand(program, args, stop, result);
    // systemctl status exits non-zero for a stopped unit, and journalctl for some warnings; both still print.
    if (run == RunCode::Ok && (result.exitCode == 0 || !result.output.empty() || (!journal && !result.errors.empty())))
    {
        status = "ok";
        payload = result.output.empty() && !journal ? result.errors : result.output;
        return;
    }

    payload = RunFailure(program, run, result);
}

void Reply(zmq::socket_t& socket, const std::vector<zmq::message_t>& envelope, const std::string& status,
           const std::string& payload)
{
    for (const zmq::message_t& frame : envelope)
    {
        socket.send(zmq::buffer(frame.data(), frame.size()), zmq::send_flags::sndmore | zmq::send_flags::dontwait);
    }

    socket.send(zmq::buffer(status), zmq::send_flags::sndmore | zmq::send_flags::dontwait);
    socket.send(zmq::buffer(payload), zmq::send_flags::dontwait);
}

} // namespace

struct JournalServer::Shared
{
    ServerSettings settings;
    std::atomic<bool> stop{false};
    zmq::context_t context{1};
    // Made and bound by Start(); from then on only the serving thread uses it.
    std::unique_ptr<zmq::socket_t> socket;
    std::string bound;
};

JournalServer::JournalServer(std::string endpoint, ManagerIdentity manager, std::string journalctl, std::string systemctl)
    : shared{std::make_shared<Shared>()}
{
    shared->settings.endpoint = std::move(endpoint);
    shared->settings.manager = std::move(manager);
    shared->settings.journalctl = std::move(journalctl);
    shared->settings.systemctl = std::move(systemctl);
}

JournalServer::~JournalServer()
{
    Stop();
}

JournalServer::JournalServer(JournalServer&& other) noexcept = default;

JournalServer& JournalServer::operator=(JournalServer&& other) noexcept
{
    if (this != &other)
    {
        Stop();
        shared = std::move(other.shared);
        worker = std::move(other.worker);
    }

    return *this;
}

StartCode JournalServer::Start(std::string& error)
{
    if (!shared)
    {
        error = "moved from";
        return StartCode::BindFailed;
    }

    if (worker.joinable())
    {
        return StartCode::Ok;
    }

    try
    {
        auto socket = std::make_unique<zmq::socket_t>(shared->context, zmq::socket_type::router);
        socket->set(zmq::sockopt::linger, 0);
        socket->bind(shared->settings.endpoint);
        shared->bound = socket->get(zmq::sockopt::last_endpoint);
        shared->socket = std::move(socket);
    }
    catch (const zmq::error_t& e)
    {
        error = e.what();
        return StartCode::BindFailed;
    }

    // Starting the thread is a full memory barrier, so the socket can move to it.
    worker = std::thread{Serve, shared};
    return StartCode::Ok;
}

std::string JournalServer::BoundEndpoint() const
{
    return shared ? shared->bound : std::string{};
}

void JournalServer::Serve(std::shared_ptr<Shared> shared)
{
    zmq::socket_t& socket = *shared->socket;
    while (!shared->stop)
    {
        try
        {
            zmq::pollitem_t items[] = {{socket.handle(), 0, ZMQ_POLLIN, 0}};
            zmq::poll(items, 1, poll_period);
            if ((items[0].revents & ZMQ_POLLIN) == 0)
            {
                continue;
            }

            std::vector<zmq::message_t> frames{};
            if (!zmq::recv_multipart(socket, std::back_inserter(frames), zmq::recv_flags::dontwait) || frames.empty())
            {
                continue;
            }

            // The ROUTER puts the client's identity first; a REQ client adds an empty frame after it.
            const std::size_t envelopeSize = frames.size() > 1 && frames[1].size() == 0 ? 2 : 1;
            std::vector<zmq::message_t> envelope{};
            std::vector<std::string> request{};
            for (std::size_t i = 0; i < frames.size(); ++i)
            {
                if (i < envelopeSize)
                {
                    envelope.push_back(std::move(frames[i]));
                }
                else
                {
                    request.push_back(frames[i].to_string());
                }
            }

            std::string status{};
            std::string payload{};
            Handle(shared->settings, shared->stop, request, status, payload);
            Reply(socket, envelope, status, payload);
        }
        catch (const zmq::error_t& e)
        {
            if (e.num() == ETERM)
            {
                break;
            }
        }
    }

    shared->socket.reset();
}

void JournalServer::Stop()
{
    if (shared)
    {
        shared->stop = true;
    }

    if (worker.joinable())
    {
        worker.join();
    }
}

} // namespace journal_server
