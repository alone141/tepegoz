#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <queue>

#include "ProcessManager.hpp"
#include "Pipe.hpp"

#include <zmq_addon.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <unistd.h>
#include <vector>

#include "ProcessGroupBuilder.hpp"
#include "ConfigSection.hpp"
#include "ConfigSource.hpp"
#include "GpuMonitor.hpp"
#include "JournalServer.hpp"
#include "Logger.hpp"
#include "ReloadPlan.hpp"

namespace process_manager
{

namespace
{

constexpr int max_messages_per_cycle = 64;
// A GPU sample this old means the sampling thread is stuck in a driver call.
constexpr std::chrono::milliseconds gpu_sample_stale_after{ 5'000 };
// Reports go out on this schedule, not on every pass: each heartbeat wakes the loop, so a report per pass made
// every subscriber's traffic grow with the square of the number of services.
constexpr std::chrono::milliseconds report_period{ 500 };

// Whether two reports show the same services in the same states. A change goes out at once instead of at the
// next scheduled report, so a start, stop or restart is seen even when it is over within one period.
bool SameStates(const std::vector<DetailedHealthReport>& a, const std::vector<DetailedHealthReport>& b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
        [](const DetailedHealthReport& x, const DetailedHealthReport& y)
        {
            return x.pid == y.pid && x.runtimeHealth.state == y.runtimeHealth.state
                && x.runtimeHealth.restartCount == y.runtimeHealth.restartCount
                && x.runtimeHealth.restartToApply == y.runtimeHealth.restartToApply
                && std::strncmp(x.processName, y.processName, sizeof(x.processName)) == 0;
        });
}

std::string FixedString(const char* text, size_t capacity)
{
    return std::string(text, ::strnlen(text, capacity));
}

std::vector<std::string> NamesOf(const std::vector<Process>& processes)
{
    std::vector<std::string> names;
    for (const auto& process : processes) names.push_back(process.GetProcessName());
    return names;
}

bool Contains(const std::vector<std::string>& names, const std::string& name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

uint16_t Count(const std::vector<std::string>& names)
{
    return static_cast<uint16_t>(std::min<size_t>(names.size(), std::numeric_limits<uint16_t>::max()));
}

// A warning for names whose new definition could not be read, otherwise an information.
void LogNames(bool warning, const char* what, const std::vector<std::string>& names)
{
    if (names.empty()) return;
    std::string joined;
    for (const auto& name : names) joined += (joined.empty() ? "" : ", ") + name;
    if (warning) logger::Warning("Reload: %s: %s", what, joined.c_str());
    else logger::Info("Reload: %s: %s", what, joined.c_str());
}

// The manager's unit, and the cgroup that holds task_pm and every task_<name>, as journald records them.
journal_server::ManagerIdentity JournalIdentity()
{
    const std::filesystem::path parent = ProcessGroupBuilder::GetInstance().GetParentGroupPath();
    const std::filesystem::path relative = parent.lexically_relative("/sys/fs/cgroup");
    const bool inside = !relative.empty() && relative != "." && *relative.begin() != "..";
    return journal_server::IdentityFromCgroup(inside ? "/" + relative.generic_string() : "/", ::getpid());
}

// A process on a GPU, with what tells which service it belongs to.
struct GpuProcess
{
    pid_t pid;
    pid_t pgid;
    std::filesystem::path cgroup;
    gpu_monitor::ProcessGpuUsage usage;
};

// Turns the GPU samples, which are per process, into GPU use per service for the health reports.
class GpuUsageReporter
{
public:
    void Update(const gpu_monitor::GpuSnapshot& gpu)
    {
        state = GetState(gpu);
        if (state != loggedState)
        {
            Log(gpu);
            loggedState = state;
        }
        if (gpu.takenAt == takenAt) return; // the sample of the last pass
        takenAt = gpu.takenAt;
        processes.clear();
        for (const auto& entry : gpu.processes)
        {
            processes.push_back({ entry.first, ::getpgid(entry.first), ProcessGroupBuilder::GetCgroupPathOf(entry.first), entry.second });
        }
    }

    GpuUsage GetUsage(const Process& process) const
    {
        GpuUsage usage{};
        usage.state = state;
        if (state != GpuUsageState::Sampled) return usage;

        bool utilizationKnown = false;
        bool utilizationMissing = false;
        int32_t utilization = 0;
        for (const auto& gpuProcess : processes)
        {
            if (!process.IsMember(gpuProcess.pid, gpuProcess.pgid, gpuProcess.cgroup)) continue;
            usage.memoryUsageInBytes += gpuProcess.usage.memoryBytes;
            if (gpuProcess.usage.utilizationPercent == gpu_monitor::utilization_unknown)
            {
                utilizationMissing = true;
                continue;
            }
            utilizationKnown = true;
            utilization += gpuProcess.usage.utilizationPercent;
        }
        // Unknown only when the driver reports it for none of its GPU processes; without any it uses none.
        usage.utilizationPercent = utilizationMissing && !utilizationKnown ? -1 : utilization;
        return usage;
    }

private:
    static GpuUsageState GetState(const gpu_monitor::GpuSnapshot& gpu)
    {
        if (gpu.status == gpu_monitor::GpuStatus::NotSampled) return GpuUsageState::NotSampled;
        if (std::chrono::steady_clock::now() - gpu.takenAt > gpu_sample_stale_after) return GpuUsageState::Stalled;
        switch (gpu.status)
        {
        case gpu_monitor::GpuStatus::Ok:
            return GpuUsageState::Sampled;
        case gpu_monitor::GpuStatus::LibraryMissing:
            return GpuUsageState::NoDriver;
        case gpu_monitor::GpuStatus::NoDevice:
            return GpuUsageState::NoDevice;
        default:
            return GpuUsageState::Failed; // InitFailed or QueryFailed
        }
    }

    void Log(const gpu_monitor::GpuSnapshot& gpu) const
    {
        switch (state)
        {
        case GpuUsageState::Sampled:
            logger::Info("GPU use is sampled through %s", gpu_monitor::nvml_library);
            break;
        case GpuUsageState::NoDriver:
            logger::Info("GPU use is not reported: %s could not be loaded (no NVIDIA driver)", gpu_monitor::nvml_library);
            break;
        case GpuUsageState::NoDevice:
            logger::Info("GPU use is not reported: NVML finds no GPU");
            break;
        case GpuUsageState::Failed:
            logger::Error("GPU use is not reported: NVML failed with error %d", gpu.nvmlError);
            break;
        case GpuUsageState::Stalled:
            logger::Error("GPU use is not reported: no GPU sample for %lld ms, a driver call is not returning",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - gpu.takenAt).count()));
            break;
        default:
            break;
        }
    }

    GpuUsageState state = GpuUsageState::NotSampled;
    GpuUsageState loggedState = GpuUsageState::NotSampled;
    std::chrono::steady_clock::time_point takenAt{};
    std::vector<GpuProcess> processes;
};

} // namespace

int ProcessManager::StartUp()
{
    ProcessGroupBuilder::GetInstance().GenerateGroups();

    auto cycleDetected = !SortByDependencies(errorList, processParams->processList);
    CheckProcessConfig(errorList);

    if (!errorList.empty())
    {

        logger::Error("================================================");
        logger::Error("PLEASE FIX MENTIONED WARNINGS IN THE CONFIG FILE");
        for (const auto& error : errorList)
        {
            logger::Error("%s", error.c_str());
        }
        logger::Error("================================================");

    }
    if (cycleDetected)
    {
        logger::Error("!!!PROCESS MANAGER CANNOT START WITH CYCLE DEPENDENCIES!!!");
        return 0;
    }
    return 1;
}

ProcessManager::ProcessManager(std::unique_ptr<ProcessManagerParams> args)
    :
    zmqContext(1),
    listener(zmqContext, zmq::socket_type::dealer),
    statusPublisher(zmqContext, zmq::socket_type::pub),
    registrar(zmqContext, zmq::socket_type::router)
{
    listener.set(zmq::sockopt::routing_id, comm::process_manager_id);
    listener.connect(comm::command_server_port);

    statusPublisher.bind(comm::health_publish_port_detailed);

    processParams = std::move(args);
    BindRegistry();
}
ProcessManager::~ProcessManager()
{
    //shutdown
    listener.close();
    statusPublisher.close();
    registrar.close();
    zmqContext.close();
}
void ProcessManager::Run()
{
    logger::Info("Health status will be published from %s", comm::health_publish_port_detailed);
    logger::Info("Heartbeats will be listened from %s as %s", comm::command_server_port, comm::process_manager_id.c_str());
    // The broker learns a dealer's name from what it sends, and this socket otherwise only receives:
    // a message to itself names it in the broker's log. It comes back here and is ignored.
    listener.send(zmq::buffer(comm::process_manager_id), zmq::send_flags::sndmore | zmq::send_flags::dontwait);
    listener.send(zmq::str_buffer("hello"), zmq::send_flags::dontwait);
    zmq::pollitem_t items[] = { { listener.handle(), 0, ZMQ_POLLIN, 0}, { registrar.handle(), 0, ZMQ_POLLIN, 0} };

    // Sampled on a thread of its own: a driver call that hangs must not hold up the supervision.
    gpu_monitor::GpuMonitor gpuMonitor{ gpu_monitor::LoadNvml, gpu_monitor::default_period, gpu_monitor::default_retry_period };
    GpuUsageReporter gpuUsage;
    // Reads the journal for a monitor GUI on another machine, which cannot run journalctl here. Without it the
    // manager still supervises; only such a GUI loses its logs.
    journal_server::JournalServer journalServer{ comm::journal_server_port, JournalIdentity(),
        std::string(journal_server::default_journalctl), std::string(journal_server::default_systemctl) };
    std::string journalError;
    if (journalServer.Start(journalError) == journal_server::StartCode::Ok)
    {
        logger::Info("Journal reads will be served from %s", comm::journal_server_port);
    }
    else
    {
        logger::Error("Journal server could not bind %s: %s; a GUI on another machine will show no logs", comm::journal_server_port, journalError.c_str());
    }
    std::vector<DetailedHealthReport> published;
    auto nextReport = std::chrono::steady_clock::now();
    while (enabled)
    {
        StartSequentialAsync();

        gpuUsage.Update(gpuMonitor.Latest());
        std::vector<DetailedHealthReport> status;
        for (auto& process : processParams->processList)
        {
            process.CheckHeartbeat();
            process.AdvanceStop();
            process.ReapExit();
            process.DrainStdioPipe();
            status.emplace_back(process.GetHealthReport());
            status.back().gpuUsage = gpuUsage.GetUsage(process);
        }
        for (auto& process : retiring)
        {
            process.AdvanceStop();
            process.ReapExit();
            process.DrainStdioPipe();
        }
        retiring.erase(std::remove_if(retiring.begin(), retiring.end(),
            [](const Process& p) { return !p.HasPid() && !p.IsStopping(); }), retiring.end());

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextReport || !SameStates(status, published))
        {
            zmq::message_t detailedReport(status.data(), sizeof(DetailedHealthReport) * status.size());
            statusPublisher.send(detailedReport, zmq::send_flags::none);

            published = std::move(status);
            nextReport = now + report_period;
        }

        // Pipe output is drained once per pass above, so a pass comes at least every 500 ms. The wait also ends
        // when the next report is due.
        const auto longest = IsStopInProgress() ? std::chrono::milliseconds(50) : std::chrono::milliseconds(500);
        const auto untilReport = std::chrono::ceil<std::chrono::milliseconds>(nextReport - std::chrono::steady_clock::now());
        const auto timeout = std::clamp(untilReport, std::chrono::milliseconds(0), longest);
        try
        {
            zmq::poll(items, 2, timeout);
        }
        catch (const zmq::error_t& e)
        {
            if (e.num() != EINTR) throw;
        }

        // Take every queued message, so a backlog never makes a healthy process look silent.
        for (int i = 0; i < max_messages_per_cycle; ++i)
        {
            std::vector<zmq::message_t> messages;
            try
            {
                if (!zmq::recv_multipart(listener, std::back_inserter(messages), zmq::recv_flags::dontwait)) break;
            }
            catch (const std::exception& e)
            {
                logger::Debug("%s", e.what());
                break;
            }
            ProcessMessage(messages);
        }
        ServeRegistry();
    }
    logger::Debug("Exiting");
    StopAll(); // here, on the thread that supervises the processes

    return;
}
void ProcessManager::RequestStop()
{
    enabled = false;
}
int ProcessManager::TearDown()
{
    enabled = false;
    logger::Debug("TearDown");
    StopAll();
    return 0;
}
// Blocking, for shutdown: every process gets SIGTERM at once, then all are waited for together.
void ProcessManager::StopAll()
{
    for (auto& process : processParams->processList)
    {
        process.SuspendHealthCheck();
        process.Stop();
    }
    for (auto& process : retiring)
    {
        process.Stop();
    }
    while (IsStopInProgress())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (auto& process : processParams->processList) process.AdvanceStop();
        for (auto& process : retiring) process.AdvanceStop();
    }
}
void ProcessManager::RetireProcess(Process& process, std::vector<Process>& retiring)
{
    process.SuspendHealthCheck();
    process.Stop();
    if (process.HasPid() || process.IsStopping()) retiring.push_back(std::move(process));
}
bool ProcessManager::IsStopInProgress() const
{
    for (const auto& process : processParams->processList)
    {
        if (process.IsStopping()) return true;
    }
    for (const auto& process : retiring)
    {
        if (process.IsStopping()) return true;
    }
    return false;
}
std::unique_ptr<ProcessManagerParams> ProcessManager::GenerateConfig(const config_section::ConfigValue& document, std::vector<std::string>* errors)
{
    auto processManagerParams = std::make_unique<ProcessManagerParams>();
    processManagerParams->document = document;
    std::vector<std::string> problems;
    const auto addProblems = [&problems](const std::vector<std::string>& found) { problems.insert(problems.end(), found.begin(), found.end()); };

    config_section::ConfigSection top(document, "the configuration");
    bool hasDefaultWorkMode = false;
    if (const config_section::ConfigValue* managerValue = top.ReadSection("ProcessManager"))
    {
        config_section::ConfigSection manager(*managerValue, "ProcessManager");
        // Inherit: no default, so a service without a WorkMode of its own is Manual.
        const int mode = manager.ReadChoice("Mode", { "Inherit", "Manual", "Auto" }, 0);
        hasDefaultWorkMode = mode != 0;
        processManagerParams->remoteRegistration = manager.ReadBool("RemoteRegistration", false);
        processManagerParams->workMode = mode == 2 ? WorkMode::Auto : WorkMode::Manual;
        // The choices in logger::Level order.
        processManagerParams->logLevel = static_cast<logger::Level>(manager.ReadChoice("LogLevel", { "Verbose", "Debug", "Info", "Warning", "Error" }, 1));
        addProblems(manager.Problems());
    }
    const config_section::ConfigValue* services = top.ReadSequence("Services");
    addProblems(top.Problems());

    for (size_t i = 0; services != nullptr && i < services->children.size(); i++)
    {
        const config_section::ConfigValue& item = services->children[i];
        const config_section::ConfigValue* name = config_section::FindValue(item, "ProcessName");
        const std::string label = name != nullptr && !name->text.empty() ? "service '" + name->text + "'" : "service " + std::to_string(i + 1);
        config_section::ConfigSection section(item, label);
        if (!section.ReadBool("Enabled", true)) continue; // switched off: nothing else of it is read or checked

        Process process;
        const int loaded = process.UpdateArgs(section);
        if (loaded == -2)
        {
            problems.push_back(label + ": ProcessName is reserved (pm, all, BPM, PMC, GUI) or contains '/'; service ignored");
            continue;
        }
        if (loaded == -3)
        {
            addProblems(section.Problems());
            problems.push_back(label + ": a renamed key is set in both its spellings; service ignored");
            continue;
        }
        if (loaded != 1)
        {
            problems.push_back(label + ": ProcessName and ExecutablePath are required; service ignored");
            continue;
        }
        addProblems(section.Problems());
        // [ProcessManager] Mode applies to every service without a WorkMode of its own.
        if (hasDefaultWorkMode && !section.Has("WorkMode"))
        {
            process.SetWorkMode(processManagerParams->workMode);
        }
        processManagerParams->processList.emplace_back(std::move(process));
    }

    for (const auto& problem : problems)
    {
        if (errors) errors->push_back(problem);
        else logger::Error("%s", problem.c_str());
    }

    return processManagerParams;
}
std::unique_ptr<ProcessManagerParams> ProcessManager::RebuildConfig(const ProcessManagerParams& params)
{
    std::unique_ptr<ProcessManagerParams> rebuilt = GenerateConfig(params.document);
    rebuilt->source = params.source;
    rebuilt->origins = params.origins;
    return rebuilt;
}
ReloadReport ProcessManager::Reload(ProcessManagerParams& params, std::vector<Process>& retiring)
{
    ReloadReport report{};
    report.command = CommandCode::ReloadConfig;
    const auto refuse = [&report](ReloadResult result, const std::string& why)
    {
        logger::Error("Reload refused, nothing changed: %s", why.c_str());
        report.result = result;
        return report;
    };
    const std::string mainFile = params.source.MainFile().string();
    if (mainFile.empty()) return refuse(ReloadResult::NoSource, "the configuration was not read from a file");

    config_source::LoadedConfig loaded;
    std::string error;
    const config_source::LoadCode read = params.source.Load(loaded, error);
    if (read == config_source::LoadCode::MainFileNotOpened) return refuse(ReloadResult::MainFileNotOpened, mainFile + " could not be opened");
    if (read == config_source::LoadCode::MainFileSyntaxError) return refuse(ReloadResult::MainFileSyntaxError, mainFile + " is not valid YAML: " + error);

    // What start-up would make of the new configuration.
    std::vector<std::string> problems = loaded.problems;
    auto next = GenerateConfig(loaded.document, &problems);
    std::vector<std::string> orderErrors;
    if (!SortByDependencies(orderErrors, next->processList))
    {
        for (const auto& orderError : orderErrors) logger::Error("%s", orderError.c_str());
        return refuse(ReloadResult::DependencyCycle, "the new configuration has a dependency cycle");
    }

    reload_plan::ReloadPlan plan;
    switch (reload_plan::PlanReload(NamesOf(params.processList), params.document, loaded, NamesOf(next->processList), plan))
    {
    case reload_plan::PlanCode::FolderNotReadable:
        return refuse(ReloadResult::FolderNotReadable, params.source.DropInFolder().string() + " exists but could not be listed");
    case reload_plan::PlanCode::NoServicesList:
        return refuse(ReloadResult::NoServicesList, mainFile + " has no Services list for the services: its top level must be a mapping, and its Services a list");
    case reload_plan::PlanCode::DuplicateName:
        return refuse(ReloadResult::DuplicateName, "the new configuration has two services of the same name");
    default:
        break;
    }

    problems.insert(problems.end(), orderErrors.begin(), orderErrors.end()); // missing dependencies
    for (const auto& process : next->processList)
    {
        const std::string name = process.GetProcessName();
        if (Contains(plan.added, name) || Contains(plan.changed, name)) process.CheckConfig(problems);
    }
    for (const auto& problem : problems) logger::Error("%s", problem.c_str());

    // The new list is built by moving each Process into it: a move hands its child over, and nothing is stopped.
    // Kept services come first, in their order, then the new configuration's start order.
    std::vector<Process>& current = params.processList;
    std::unordered_map<std::string, size_t> index;
    for (size_t i = 0; i < current.size(); ++i) index.emplace(current[i].GetProcessName(), i);
    std::vector<char> placed(current.size(), 0);
    std::vector<Process> list;
    list.reserve(current.size() + next->processList.size());
    for (const auto& name : plan.kept)
    {
        const size_t i = index.at(name);
        list.push_back(std::move(current[i]));
        placed[i] = 1;
    }
    for (auto& process : next->processList)
    {
        const auto found = index.find(process.GetProcessName());
        if (found == index.end())
        {
            list.push_back(std::move(process)); // added: started by its work mode, as at start-up
            continue;
        }
        Process& existing = current[found->second];
        if (Contains(plan.changed, process.GetProcessName())) existing.DeferDefinition(process);
        list.push_back(std::move(existing));
        placed[found->second] = 1;
    }
    for (size_t i = 0; i < current.size(); ++i)
    {
        if (placed[i]) continue;
        if (Contains(plan.removed, current[i].GetProcessName())) RetireProcess(current[i], retiring);
        else list.push_back(std::move(current[i])); // a second service of the same name, from start-up: kept
    }
    current = std::move(list);

    params.document = std::move(loaded.document);
    params.origins = std::move(loaded.origins);
    params.workMode = next->workMode;
    params.logLevel = next->logLevel;
    logger::Logger::Instance().SetMinimum(params.logLevel);

    logger::Info("Configuration reloaded from %s: %zu added, %zu removed, %zu changed, %zu kept, %zu not added, %zu unchanged",
        mainFile.c_str(), plan.added.size(), plan.removed.size(), plan.changed.size(), plan.kept.size(), plan.skipped.size(), plan.unchanged.size());
    LogNames(false, "added", plan.added);
    LogNames(false, "removed", plan.removed);
    LogNames(false, "changed, the new definition waits for the next start", plan.changed);
    LogNames(true, "kept as they are, their new definition could not be read", plan.kept);
    LogNames(true, "not added, their definition could not be read", plan.skipped);
    if (plan.unnamedEntry)
    {
        logger::Warning("Reload: a Services entry has no ProcessName that can be read and could be any service, so none was removed");
    }

    report.result = ReloadResult::Applied;
    report.added = Count(plan.added);
    report.removed = Count(plan.removed);
    report.changed = Count(plan.changed);
    report.kept = Count(plan.kept);
    report.skipped = Count(plan.skipped);
    report.unchanged = Count(plan.unchanged);
    return report;
}
bool ProcessManager::SortByDependencies(std::vector<std::string>& errors, std::vector<Process>& pList)
{
    std::unordered_map<std::string, size_t> idx;

    idx.reserve(pList.size());
    for (size_t i = 0; i < pList.size(); ++i)
    {
        if (idx.find(pList[i].GetProcessName()) != idx.end())
        {
            errors.push_back("duplicate process name: " + pList[i].GetProcessName());
            continue;
        }
        idx[pList[i].GetProcessName()] = i;
    }

    //Kahn's Algorithm
    // Build graph: dep -> dependents
    std::vector<std::vector<size_t>> adj(pList.size());
    std::vector<int> indeg(pList.size(), 0);

    for (size_t i = 0; i < pList.size(); ++i)
    {
        for (const auto& dep : pList[i].GetDependencyList())
        {
            auto it = idx.find(dep);
            if (it == idx.end())
            {
                errors.push_back("missing dependency: " + pList[i].GetProcessName() + " depends on '" + dep + "'");
                continue; // ignore unknown deps so we can still order what we know
            }
            adj[it->second].push_back(i); // dep -> i
            indeg[i] += 1;
        }
    }

    std::queue<size_t> q;
    for (size_t i = 0; i < pList.size(); ++i)
    {
        if (indeg[i] == 0) q.push(i);
    }

    std::vector<size_t> order;
    order.reserve(pList.size());

    while (!q.empty())
    {
        size_t u = q.front(); q.pop();
        order.push_back(u);
        for (size_t v : adj[u])
        {
            if (--indeg[v] == 0) q.push(v);
        }
    }

    // Cycle detection
    if (order.size() != pList.size())
    {
        std::string cyc = "cycle detected among: ";
        bool first = true;
        for (size_t i = 0; i < pList.size(); ++i)
        {
            if (indeg[i] > 0)
            {
                if (!first) cyc += ", ";
                cyc += pList[i].GetProcessName();
                first = false;
            }
        }
        errors.push_back(cyc);
    }

    std::vector<Process> out;
    out.reserve(pList.size());

    std::vector<char> used(pList.size(), 0);
    for (size_t i : order) { out.push_back(std::move(pList[i])); used[i] = 1; }
    for (size_t i = 0; i < pList.size(); ++i)
    {
        if (!used[i]) out.push_back(std::move(pList[i]));
    }


    pList = std::move(out);

    return order.size() == pList.size(); // true => no cycles

}

bool ProcessManager::CheckProcessConfig(std::vector<std::string>& err) const
{
    for (const auto& process : processParams->processList)
    {
        process.CheckConfig(err);
    }
    return false;
}

bool ProcessManager::ApplyToAll(CommandCode command)
{
    if (command == CommandCode::StopService)
    {
        for (auto& process : processParams->processList)
        {
            process.SetWorkMode(WorkMode::Manual);
            process.SuspendHealthCheck();
            process.Stop();
        }
        return true;
    }
    if (command == CommandCode::StartService || command == CommandCode::RestartService)
    {
        logger::Debug("--start all received, resetting config");
        for (auto& process : processParams->processList)
        {
            RetireProcess(process, retiring); // the new instances start once these have exited
        }
        processParams = RebuildConfig(*processParams); //reset the runtime params

        std::vector<std::string> errors;
        SortByDependencies(errors, processParams->processList);
        for (const auto& error : errors)
        {
            logger::Error("%s", error.c_str());
        }
        for (auto& process : processParams->processList)
        {
            process.SetWorkMode(WorkMode::Auto);
        }
        return true;
    }
    return false;
}

void ProcessManager::StartDependencies(const Process& process) const
{
    for (const auto& dependsOn : process.GetDependencyList())
    {
        auto dependency = GetProcess(dependsOn);
        if (dependency == processParams->processList.end())
        {
            logger::Error("[%s] Cannot start missing dependency %s", process.GetProcessName().c_str(), dependsOn.c_str());
            continue;
        }
        dependency->StartProcess(); // no-op while it runs
    }
}

bool ProcessManager::ApplyCommandServerMessage(const CommandMessage& message)
{
    logger::Debug("Command received");
    const auto& command = message.command;
    const std::string processName = FixedString(message.serviceName, sizeof(message.serviceName));
    if (processName == "all")
    {
        return ApplyToAll(command);
    }
    auto it = GetProcess(processName);
    if (it == processParams->processList.end())
    {
        logger::Warning("Cannot find %s", processName.c_str());
        return false;
    }
    else if (command == CommandCode::StartService)
    {
        // A waiting definition is taken before the start, so that the start keeps it Auto.
        if (!it->HasPid() && !it->IsStopping()) it->ApplyDeferredDefinition();
        StartDependencies(*it);
        it->SetWorkMode(WorkMode::Auto);
        if (it->IsStopping())
        {
            it->Restart(); // starts again once the stop in progress finishes
        }
        else
        {
            it->StartProcess(); // no-op while it runs
        }
    }
    else if (command == CommandCode::StartServiceWithArgs)
    {
        if (it->HasPid())
        {
            logger::Error("[%s] Already running, stop it before starting it with other arguments", processName.c_str());
            return false;
        }
        const std::string argsText = FixedString(message.args, sizeof(message.args));
        it->OverrideStartupArgs(config_section::SplitList(argsText, ' '));

        it->StartProcess();
        it->SetWorkMode(WorkMode::Auto);
    }
    else if (command == CommandCode::StopService)
    {
        it->SetWorkMode(WorkMode::Manual);
        it->SuspendHealthCheck();
        it->Stop();
    }
    else if (command == CommandCode::RestartService)
    {
        if (!it->HasPid() && !it->IsStopping()) it->ApplyDeferredDefinition(); // as for StartService
        StartDependencies(*it);
        it->SetWorkMode(WorkMode::Auto);
        if (it->HasPid())
        {
            it->Restart();
        }
        else
        {
            it->StartProcess();
        }
    }
    else if (command == CommandCode::DisableService)
    {
        RetireProcess(*it, retiring);
        processParams->processList.erase(it);
    }
    else
    {
        return false;
    }
    return true;
}

bool ProcessManager::ProcessMessage(const std::vector<zmq::message_t>& messages)
{
    if (messages.size() < 2) return false;

    const auto& receivedFrom = messages[0].to_string_view();
    if (receivedFrom == comm::command_server_id ||
        receivedFrom == comm::gui_id)
    {
        const auto& payload = messages[1];
        if (payload.size() == 0) return false;
        const auto command = static_cast<CommandCode>(*static_cast<const char*>(payload.data()));
        if (command == CommandCode::GetVersionInfo)
        {
            VersionReport version{};
            version.command = CommandCode::GetVersionInfo;
#ifdef PROCESS_MANAGER_VERSION_MAJOR
            version.manager.major = PROCESS_MANAGER_VERSION_MAJOR;
            version.manager.minor = PROCESS_MANAGER_VERSION_MINOR;
            version.manager.patch = PROCESS_MANAGER_VERSION_PATCH;
#endif // PROCESS_MANAGER_VERSION_MAJOR


            auto result = reinterpret_cast<char*>(&version);
            listener.send(zmq::buffer(comm::gui_id), zmq::send_flags::sndmore);
            listener.send(zmq::buffer(result, sizeof(VersionReport)), zmq::send_flags::none);
            return true;
        }
        else if (command == CommandCode::ReloadConfig)
        {
            CommandMessage message{};
            std::memcpy(&message, payload.data(), std::min(payload.size(), sizeof(message)));
            logger::Info("Reload asked by %s", std::string(receivedFrom).c_str());
            const ReloadReport report = Reload(*processParams, retiring);
            if (FixedString(message.args, sizeof(message.args)) != reload_without_reply)
            {
                listener.send(zmq::buffer(receivedFrom), zmq::send_flags::sndmore); // the sender: PMC or GUI
                listener.send(zmq::buffer(&report, sizeof(report)), zmq::send_flags::none);
            }
            return true;
        }
        else
        {
            // A short frame leaves the rest zero; a longer one is cut to the struct.
            CommandMessage message{};
            std::memcpy(&message, payload.data(), std::min(payload.size(), sizeof(message)));
            auto reply = ApplyCommandServerMessage(message);
            auto resultString = std::to_string(reply);
            if (messages[0].to_string_view() == comm::gui_id)
            {
                listener.send(zmq::buffer(comm::gui_id), zmq::send_flags::sndmore);
                listener.send(zmq::buffer(resultString, 1), zmq::send_flags::none);
            }
            return true;

        }
    }
    else
    {
        if (auto it = GetProcess(receivedFrom); it != processParams->processList.end())
        {
            it->HeartbeatReceived();
            return true;
        }
        return false;
    }
}

bool ProcessManager::StartSequentialAsync() const
{
    for (const auto& process : retiring)
    {
        if (process.IsStopping()) return false; // replaced instances exit before anything new starts
    }
    for (auto& process : processParams->processList)
    {
        if (process.IsRestartRequested() && !process.HasPid() && !process.IsStopping())
        {
            process.StartProcess();
            return false;
        }
        if (process.IsWorkingManually()) continue;
        if (process.IsStopping()) return false;
        if (process.IsGracePeriod()) break;
        if (process.HasPid())
        {
            if (!process.NeedsRestart()) continue;
            process.StopUnresponsive(); // the restart policy starts it again once it has exited
            return false;
        }
        if (process.NeedsStart())
        {
            process.StartProcess();
            return false;
        }
        if (process.NeedsRestart())
        {
            process.AutoRestart();
            return false;
        }


    }
    return true;
}

std::vector<Process>::iterator ProcessManager::GetProcess(std::string_view processName) const
{
    const auto& listStart = processParams->processList.begin();
    const auto& listEnd = processParams->processList.end();

    auto it = std::find_if(listStart, listEnd,
        [&](const Process& p)
        {
            return p.GetProcessName() == processName;
        });

    return it;
}

} // namespace process_manager
