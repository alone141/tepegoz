#include "StatusClient.hpp"
#include "CommandWatch.hpp"
#include "ProcessManagerCommands.hpp"
#include "RuntimeHealth.hpp"
#include "StatusTable.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>
#include <zmq.hpp>
#include <zmq_addon.hpp>

namespace status_client
{

namespace
{

using Clock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds poll_step{200};
// Longer than the manager's report period: an empty report this long after the reload's answer was made after it.
constexpr std::chrono::milliseconds reload_report_wait{600};
constexpr std::string_view clear_screen = "\033[H\033[2J";

static_assert(static_cast<int>(process_manager::RuntimeState::Unhealthy) ==
                  static_cast<int>(status_table::ServiceState::Unhealthy),
              "ServiceState follows the manager's RuntimeState");
static_assert(static_cast<int>(process_manager::GpuUsageState::Stalled) ==
                  static_cast<int>(status_table::GpuState::Stalled),
              "GpuState follows the manager's GpuUsageState");

std::int64_t SinceEpochNs(std::chrono::high_resolution_clock::time_point when)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(when.time_since_epoch()).count();
}

status_table::ServiceState StateOf(process_manager::RuntimeState state)
{
    const int value = static_cast<int>(state);
    if (value > static_cast<int>(status_table::ServiceState::Unhealthy))
    {
        return status_table::ServiceState::Unknown;
    }

    return static_cast<status_table::ServiceState>(value);
}

status_table::GpuState GpuStateOf(process_manager::GpuUsageState state)
{
    const int value = static_cast<int>(state);
    if (value > static_cast<int>(status_table::GpuState::Stalled))
    {
        return status_table::GpuState::NotSampled;
    }

    return static_cast<status_table::GpuState>(value);
}

// The same rule as the manager's log: colour only a terminal a person reads, and never with NO_COLOR.
bool UseColor()
{
    const char* term = std::getenv("TERM");
    return std::getenv("NO_COLOR") == nullptr && ::isatty(STDOUT_FILENO) == 1 &&
           (term == nullptr || std::string_view{term} != "dumb");
}

std::string HostName()
{
    char name[256]{};
    if (::gethostname(name, sizeof(name) - 1) != 0)
    {
        return "localhost";
    }

    return std::string{name};
}

zmq::socket_t Subscriber(zmq::context_t& context, const std::string& endpoint)
{
    zmq::socket_t socket{context, zmq::socket_type::sub};
    // Only the newest report counts: each holds every service.
    socket.set(zmq::sockopt::conflate, 1);
    socket.set(zmq::sockopt::linger, 0);
    socket.set(zmq::sockopt::subscribe, "");
    socket.connect(endpoint);
    return socket;
}

// Waits up to timeout for a report that parses; false when none came.
bool NextReport(zmq::socket_t& socket, std::chrono::milliseconds timeout, std::vector<status_table::ServiceSample>& out)
{
    const Clock::time_point deadline = Clock::now() + timeout;
    while (true)
    {
        const std::chrono::milliseconds left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (left.count() < 0)
        {
            return false;
        }

        zmq::pollitem_t items[] = {{socket.handle(), 0, ZMQ_POLLIN, 0}};
        zmq::poll(items, 1, std::min(left, poll_step));
        if ((items[0].revents & ZMQ_POLLIN) == 0)
        {
            continue;
        }

        zmq::message_t message{};
        if (!socket.recv(message, zmq::recv_flags::dontwait))
        {
            continue;
        }

        const std::string_view bytes{static_cast<const char*>(message.data()), message.size()};
        if (ParseReport(bytes, out) == ParseCode::Ok)
        {
            return true;
        }
    }
}

process_manager::CommandCode WireCommand(command_watch::Command command)
{
    switch (command)
    {
    case command_watch::Command::Start:
        return process_manager::CommandCode::StartService;
    case command_watch::Command::Stop:
        return process_manager::CommandCode::StopService;
    case command_watch::Command::Restart:
        return process_manager::CommandCode::RestartService;
    case command_watch::Command::Disable:
        return process_manager::CommandCode::DisableService;
    }
    return process_manager::CommandCode::Unknown;
}

bool Send(zmq::context_t& context, const std::string& endpoint, command_watch::Command command,
          std::string_view target)
{
    process_manager::CommandMessage message{};
    message.command = WireCommand(command);
    std::memcpy(message.serviceName, target.data(), target.size());

    zmq::socket_t socket{context, zmq::socket_type::dealer};
    socket.set(zmq::sockopt::routing_id, command_line_identity);
    // Give up when the manager is not running instead of waiting for it forever.
    socket.set(zmq::sockopt::immediate, 1);
    socket.set(zmq::sockopt::sndtimeo, 2000);
    socket.set(zmq::sockopt::linger, 2000);
    socket.connect(endpoint);
    const std::string& destination = process_manager::comm::process_manager_id;
    return socket.send(zmq::buffer(destination), zmq::send_flags::sndmore) &&
           socket.send(zmq::buffer(&message, sizeof(message)), zmq::send_flags::none);
}

std::string Names(const std::vector<status_table::ServiceSample>& report)
{
    std::string names{};
    for (const status_table::ServiceSample& sample : report)
    {
        names += (names.empty() ? "" : ", ") + sample.name;
    }
    return names.empty() ? std::string{"none"} : names;
}

void PrintLines(const std::vector<std::string>& lines)
{
    for (const std::string& line : lines)
    {
        std::cout << line << '\n';
    }
    std::cout << std::flush;
}

std::string RefusalText(process_manager::ReloadResult result)
{
    switch (result)
    {
    case process_manager::ReloadResult::Applied:
        return std::string{};
    case process_manager::ReloadResult::NoSource:
        return "the manager's configuration was not read from a file";
    case process_manager::ReloadResult::MainFileNotOpened:
        return "the configuration file could not be opened";
    case process_manager::ReloadResult::MainFileSyntaxError:
        return "the configuration file is not valid YAML";
    case process_manager::ReloadResult::NoServicesList:
        return "the configuration file has no Services list for the services";
    case process_manager::ReloadResult::FolderNotReadable:
        return "the services.d folder could not be listed";
    case process_manager::ReloadResult::DependencyCycle:
        return "the new configuration has a dependency cycle";
    case process_manager::ReloadResult::DuplicateName:
        return "the new configuration has two services of the same name";
    }
    return "refused with code " + std::to_string(static_cast<int>(result));
}

// The names in one report and not in the other.
std::vector<std::string> Missing(const std::vector<status_table::ServiceSample>& from,
                                 const std::vector<status_table::ServiceSample>& in)
{
    std::vector<std::string> names{};
    for (const status_table::ServiceSample& sample : from)
    {
        if (!command_watch::FindService(in, sample.name).has_value())
        {
            names.push_back(sample.name);
        }
    }
    return names;
}

std::string Joined(const std::vector<std::string>& names)
{
    std::string joined{};
    for (const std::string& name : names)
    {
        joined += (joined.empty() ? "" : ", ") + name;
    }
    return joined;
}

std::int64_t NowNs()
{
    return SinceEpochNs(std::chrono::high_resolution_clock::now());
}

// A report the manager made after it answered: one taken after the answer came, or, without services to tell the
// time by, one that came a report period after it.
bool MadeAfter(const std::vector<status_table::ServiceSample>& report, std::int64_t answeredNs,
               Clock::time_point answeredAt)
{
    if (report.empty())
    {
        return Clock::now() - answeredAt > reload_report_wait;
    }

    return std::any_of(report.begin(), report.end(),
                       [answeredNs](const status_table::ServiceSample& sample) { return sample.snapshotNs >= answeredNs; });
}

bool SendReloadRequest(zmq::socket_t& socket, bool wait)
{
    process_manager::CommandMessage message{};
    message.command = process_manager::CommandCode::ReloadConfig;
    if (!wait)
    {
        const std::string_view noReply = process_manager::reload_without_reply;
        std::memcpy(message.args, noReply.data(), noReply.size());
    }

    const std::string& destination = process_manager::comm::process_manager_id;
    return socket.send(zmq::buffer(destination), zmq::send_flags::sndmore) &&
           socket.send(zmq::buffer(&message, sizeof(message)), zmq::send_flags::none);
}

// Waits up to timeout for the answer; false when none came.
bool ReceiveReloadReply(zmq::socket_t& socket, std::chrono::milliseconds timeout, ReloadReply& out)
{
    const Clock::time_point deadline = Clock::now() + timeout;
    while (Clock::now() < deadline)
    {
        const std::chrono::milliseconds left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        zmq::pollitem_t items[] = {{socket.handle(), 0, ZMQ_POLLIN, 0}};
        zmq::poll(items, 1, std::min(left, poll_step));
        if ((items[0].revents & ZMQ_POLLIN) == 0)
        {
            continue;
        }

        std::vector<zmq::message_t> frames{};
        if (!zmq::recv_multipart(socket, std::back_inserter(frames), zmq::recv_flags::dontwait).has_value() ||
            frames.empty())
        {
            continue;
        }

        const zmq::message_t& payload = frames.back();
        const std::string_view bytes{static_cast<const char*>(payload.data()), payload.size()};
        if (ParseReloadReply(bytes, out) == ParseCode::Ok)
        {
            return true;
        }
    }
    return false;
}

} // namespace

ParseCode ParseReport(std::string_view bytes, std::vector<status_table::ServiceSample>& out)
{
    constexpr std::size_t record_size = sizeof(process_manager::DetailedHealthReport);
    if (bytes.size() % record_size != 0)
    {
        return ParseCode::BadSize;
    }

    std::vector<status_table::ServiceSample> samples{};
    for (std::size_t offset = 0; offset < bytes.size(); offset += record_size)
    {
        process_manager::DetailedHealthReport report{};
        std::memcpy(static_cast<void*>(&report), bytes.data() + offset, record_size);
        status_table::ServiceSample sample{};
        sample.name = std::string{report.processName, ::strnlen(report.processName, sizeof(report.processName))};
        sample.pid = report.pid;
        sample.state = StateOf(report.runtimeHealth.state);
        sample.missedBeats = report.runtimeHealth.missedBeats;
        sample.restarts = report.runtimeHealth.restartCount;
        sample.cpuUsec = report.cpuUsageInUsec;
        sample.memoryBytes = report.memoryUsageInBytes;
        sample.startNs = SinceEpochNs(report.runtimeHealth.startTime);
        sample.lastSeenNs = SinceEpochNs(report.runtimeHealth.lastSeen);
        sample.snapshotNs = SinceEpochNs(report.snapshotTime);
        sample.gpuState = GpuStateOf(report.gpuUsage.state);
        sample.gpuPercent = report.gpuUsage.utilizationPercent;
        sample.vramBytes = report.gpuUsage.memoryUsageInBytes;
        sample.restartToApply = report.runtimeHealth.restartToApply != 0;
        samples.push_back(sample);
    }
    out = samples;
    return ParseCode::Ok;
}

void WatchStatus(const Endpoints& endpoints)
{
    zmq::context_t context{1};
    zmq::socket_t socket = Subscriber(context, endpoints.reports);
    status_table::StatusTable table{};
    status_table::TableOptions options{};
    options.machine = HostName();
    options.color = UseColor();
    const bool terminal = ::isatty(STDOUT_FILENO) == 1;

    const Clock::time_point started = Clock::now();
    std::optional<Clock::time_point> lastReport{};
    // The first table is drawn as soon as its report comes; "Waiting" only when none came in a refresh period.
    Clock::time_point lastDrawn = started;
    bool tableDrawn = false;
    while (true)
    {
        std::vector<status_table::ServiceSample> report{};
        if (NextReport(socket, poll_step, report))
        {
            table.Update(report);
            lastReport = Clock::now();
        }

        const Clock::time_point now = Clock::now();
        const bool firstTable = lastReport.has_value() && !tableDrawn;
        if (!firstTable && now - lastDrawn < refresh_period)
        {
            continue;
        }

        lastDrawn = now;
        std::string screen{};
        if (!lastReport.has_value())
        {
            const long long waited = std::chrono::duration_cast<std::chrono::seconds>(now - started).count();
            screen = "Waiting for the process manager's reports at " + endpoints.reports + " (" +
                     std::to_string(waited) + "s)\n";
        }
        else
        {
            const std::chrono::seconds silent = std::chrono::duration_cast<std::chrono::seconds>(now - *lastReport);
            options.notice = silent >= stale_after ? "No report for " + std::to_string(silent.count()) + "s" : "";
            screen = table.Render(options);
            tableDrawn = true;
        }
        std::cout << (terminal ? clear_screen : "") << screen << (terminal ? "" : "\n") << std::flush;
    }
}

StatusCode PrintStatus(const Endpoints& endpoints)
{
    zmq::context_t context{1};
    zmq::socket_t socket = Subscriber(context, endpoints.reports);
    status_table::StatusTable table{};
    std::vector<status_table::ServiceSample> report{};
    if (!NextReport(socket, first_report_timeout, report))
    {
        std::cerr << "No report from the process manager at " << endpoints.reports << " in "
                  << std::chrono::duration_cast<std::chrono::seconds>(first_report_timeout).count()
                  << "s; is it running?\n";
        return StatusCode::NoReport;
    }

    table.Update(report);
    const Clock::time_point until = Clock::now() + cpu_sample_period;
    while (Clock::now() < until)
    {
        const std::chrono::milliseconds left =
            std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now());
        if (NextReport(socket, left, report))
        {
            table.Update(report);
        }
    }

    status_table::TableOptions options{};
    options.machine = HostName();
    options.color = UseColor();
    std::cout << table.Render(options) << std::flush;
    return StatusCode::Ok;
}

CommandCode SendCommand(command_watch::Command command, std::string_view target, const Endpoints& endpoints,
                        bool follow, std::chrono::seconds timeout)
{
    if (target.size() >= sizeof(process_manager::CommandMessage::serviceName))
    {
        std::cerr << "Service name '" << target << "' is longer than "
                  << sizeof(process_manager::CommandMessage::serviceName) - 1 << " bytes\n";
        return CommandCode::BadTarget;
    }
    if (command == command_watch::Command::Disable && target == command_watch::all_services)
    {
        std::cerr << "--disable takes one service, not all\n";
        return CommandCode::BadTarget;
    }

    zmq::context_t context{1};
    std::optional<zmq::socket_t> socket{};
    std::vector<status_table::ServiceSample> before{};
    bool watching = false;
    if (follow)
    {
        // Subscribed before sending, so the first state after the command is not missed.
        socket.emplace(Subscriber(context, endpoints.reports));
        watching = NextReport(*socket, first_report_timeout, before);
        if (!watching)
        {
            std::cerr << "No report from the process manager at " << endpoints.reports
                      << "; sending the command without following it\n";
        }
        else if (target != command_watch::all_services && !command_watch::FindService(before, target).has_value())
        {
            std::cerr << "No service named '" << target << "'. The manager has: " << Names(before) << "\n";
            return CommandCode::NoSuchService;
        }
    }

    if (!Send(context, endpoints.commands, command, target))
    {
        std::cerr << "Process manager is not reachable at " << endpoints.commands << "\n";
        return CommandCode::NotReachable;
    }
    if (!watching)
    {
        return CommandCode::Done;
    }

    command_watch::CommandWatch watch{command, target, before};
    const Clock::time_point deadline = Clock::now() + timeout;
    while (Clock::now() < deadline)
    {
        const std::chrono::milliseconds left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        std::vector<status_table::ServiceSample> after{};
        if (!NextReport(*socket, left, after))
        {
            break;
        }

        std::vector<std::string> lines{};
        const command_watch::WatchCode code = watch.Update(after, lines);
        PrintLines(lines);
        if (code == command_watch::WatchCode::Done)
        {
            return CommandCode::Done;
        }
        if (code == command_watch::WatchCode::Failed)
        {
            return CommandCode::Failed;
        }
    }

    std::vector<std::string> lines{};
    watch.DescribeWaiting(timeout, lines);
    PrintLines(lines);
    return CommandCode::TimedOut;
}

int ExitCode(CommandCode code)
{
    if (code == CommandCode::Done)
    {
        return 0;
    }

    return code == CommandCode::TimedOut ? 2 : 1;
}

ParseCode ParseReloadReply(std::string_view bytes, ReloadReply& out)
{
    process_manager::ReloadReport report{};
    if (bytes.size() != sizeof(report))
    {
        return ParseCode::BadSize;
    }

    std::memcpy(static_cast<void*>(&report), bytes.data(), sizeof(report));
    if (report.command != process_manager::CommandCode::ReloadConfig)
    {
        return ParseCode::WrongCommand;
    }

    ReloadReply reply{};
    reply.applied = report.result == process_manager::ReloadResult::Applied;
    reply.refusal = RefusalText(report.result);
    reply.added = report.added;
    reply.removed = report.removed;
    reply.changed = report.changed;
    reply.kept = report.kept;
    reply.skipped = report.skipped;
    reply.unchanged = report.unchanged;
    out = reply;
    return ParseCode::Ok;
}

std::vector<std::string> DescribeReload(const ReloadReply& reply, const std::vector<status_table::ServiceSample>& before,
                                        const std::vector<status_table::ServiceSample>& after, bool named)
{
    std::vector<std::string> lines{};
    if (!reply.applied)
    {
        lines.push_back("Reload refused: " + reply.refusal + ". Nothing was changed; the manager's log says more.");
        return lines;
    }

    lines.push_back("Reload applied: " + std::to_string(reply.added) + " added, " + std::to_string(reply.removed) +
                    " removed, " + std::to_string(reply.changed) + " changed, " + std::to_string(reply.kept) +
                    " kept, " + std::to_string(reply.skipped) + " not added, " + std::to_string(reply.unchanged) +
                    " unchanged");
    if (named)
    {
        const std::vector<std::string> added = Missing(after, before);
        const std::vector<std::string> removed = Missing(before, after);
        std::vector<std::string> waiting{};
        for (const status_table::ServiceSample& sample : after)
        {
            if (sample.restartToApply)
            {
                waiting.push_back(sample.name);
            }
        }

        if (!added.empty())
        {
            lines.push_back("  added: " + Joined(added));
        }
        if (!removed.empty())
        {
            lines.push_back("  removed: " + Joined(removed));
        }
        if (!waiting.empty())
        {
            lines.push_back("  restart to apply their new definition: " + Joined(waiting));
        }
    }
    if (reply.kept > 0)
    {
        lines.push_back("  kept as they were, their new definition could not be read: " + std::to_string(reply.kept) +
                        " (the manager's log names them)");
    }
    if (reply.skipped > 0)
    {
        lines.push_back("  not added, their definition could not be read: " + std::to_string(reply.skipped) +
                        " (the manager's log names them)");
    }
    return lines;
}

ReloadCode SendReload(const Endpoints& endpoints, bool wait, std::chrono::seconds timeout)
{
    zmq::context_t context{1};
    std::optional<zmq::socket_t> reports{};
    std::vector<status_table::ServiceSample> before{};
    bool watching = false;
    if (wait)
    {
        // Subscribed before sending, so the services before the reload are known.
        reports.emplace(Subscriber(context, endpoints.reports));
        watching = NextReport(*reports, first_report_timeout, before);
        if (!watching)
        {
            std::cerr << "No report from the process manager at " << endpoints.reports
                      << "; the reload is told without the names of the services\n";
        }
    }

    zmq::socket_t socket{context, zmq::socket_type::dealer};
    socket.set(zmq::sockopt::routing_id, command_line_identity);
    // Give up when the manager is not running instead of waiting for it forever.
    socket.set(zmq::sockopt::immediate, 1);
    socket.set(zmq::sockopt::sndtimeo, 2000);
    socket.set(zmq::sockopt::linger, wait ? 0 : 2000);
    socket.connect(endpoints.commands);
    if (!SendReloadRequest(socket, wait))
    {
        std::cerr << "Process manager is not reachable at " << endpoints.commands << "\n";
        return ReloadCode::NotReachable;
    }
    if (!wait)
    {
        return ReloadCode::Sent;
    }

    ReloadReply reply{};
    if (!ReceiveReloadReply(socket, timeout, reply))
    {
        std::cerr << "No answer from the process manager within " << timeout.count()
                  << "s; whether it reloaded is in its log\n";
        return ReloadCode::TimedOut;
    }

    const std::int64_t answeredNs = NowNs();
    const Clock::time_point answeredAt = Clock::now();
    std::vector<status_table::ServiceSample> after{};
    bool named = false;
    const Clock::time_point until = answeredAt + first_report_timeout;
    while (watching && reply.applied && !named && Clock::now() < until)
    {
        const std::chrono::milliseconds left =
            std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now());
        named = NextReport(*reports, left, after) && MadeAfter(after, answeredNs, answeredAt);
    }

    const std::vector<std::string> lines = DescribeReload(reply, before, after, named);
    if (!reply.applied)
    {
        for (const std::string& line : lines)
        {
            std::cerr << line << '\n';
        }
        return ReloadCode::Refused;
    }

    PrintLines(lines);
    return ReloadCode::Applied;
}

int ExitCode(ReloadCode code)
{
    if (code == ReloadCode::Applied || code == ReloadCode::Sent)
    {
        return 0;
    }

    return code == ReloadCode::TimedOut ? 2 : 1;
}

} // namespace status_client
