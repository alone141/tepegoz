#include "Process.hpp"
#include <unistd.h>
#include <signal.h>
#include <thread>
#include <sys/wait.h>
#include <fstream>
#include <fcntl.h>
#include "ProcessGroupBuilder.hpp"
#include "ConfigSection.hpp"
#include "Logger.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#ifndef JOURNAL_STDOUT_SOCKET
#define JOURNAL_STDOUT_SOCKET "/run/systemd/journal/stdout"
#endif

namespace fs = std::filesystem;

namespace process_manager
{

namespace
{

constexpr size_t max_stdio_line = 1024;
constexpr int max_stdio_reads = 16;
constexpr std::chrono::minutes max_restart_delay{ 5 };

// systemd sets JOURNAL_STREAM to <device>:<inode> of the journal stream behind stdout.
bool IsStdoutJournal()
{
    const char* stream = std::getenv("JOURNAL_STREAM");
    unsigned long long device = 0;
    unsigned long long inode = 0;
    if (stream == nullptr || std::sscanf(stream, "%llu:%llu", &device, &inode) != 2) return false;

    struct stat status {};
    if (::fstat(STDOUT_FILENO, &status) != 0) return false;
    return static_cast<unsigned long long>(status.st_dev) == device && static_cast<unsigned long long>(status.st_ino) == inode;
}

// The header sd_journal_stream_fd() sends: identifier, unit, priority (6, info),
// level prefix, forward to syslog, to kmsg, to console.
std::string JournalStreamHeader(const std::string& identifier)
{
    return identifier + "\n\n6\n1\n0\n0\n0\n";
}

// The functions below run in the child between fork and exec: async-signal-safe calls only.
void WriteAll(int fd, const char* data, size_t size)
{
    while (size > 0)
    {
        const ssize_t written = ::write(fd, data, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return;
        data += written;
        size -= static_cast<size_t>(written);
    }
}

void ChildError(const char* processName, const char* what)
{
    const char* reason = std::strerror(errno);
    for (const char* part : { "[", processName, "] ", what, ": ", reason, "\n" })
    {
        WriteAll(STDERR_FILENO, part, std::strlen(part));
    }
}

// stdout and stderr become a journal stream of the child's own, so journald files the
// child's lines under its PID and cgroup instead of the manager's.
bool ConnectJournalStream(const std::string& header)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, JOURNAL_STDOUT_SOCKET, sizeof(address.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        ::close(fd);
        return false;
    }
    ::shutdown(fd, SHUT_RD);
    WriteAll(fd, header.data(), header.size());
    const bool redirected = ::dup2(fd, STDOUT_FILENO) != -1 && ::dup2(fd, STDERR_FILENO) != -1;
    ::close(fd);
    return redirected;
}

} // namespace

int Process::UpdateArgs(config_section::ConfigSection& section)
{
    return config.UpdateArgs(section);
}

Process::~Process()
{
    if (runtime.processHandle.pid > 0)
    {
        RequestStopAndEscalate();
    }
}

Process::Process(Process&& other) noexcept
    : runtime(std::move(other.runtime)),
    config(std::move(other.config))
{
    other.runtime.processHandle = ProcessHandle{};
}

Process& Process::operator=(Process&& other) noexcept
{
    if (this != &other)
    {
        if (HasPid())
        {
            RequestStopAndEscalate();
        }
        runtime = std::move(other.runtime);
        config = std::move(other.config);
        other.runtime.processHandle = ProcessHandle{};
    }
    return *this;
}

void Process::StartProcess()
{
    if (HasPid())
    {
        logger::Debug("[%s] Already running as pid %d, not starting another instance", config.processName.c_str(), runtime.processHandle.pid);
        return;
    }
    runtime.consecutiveRestarts = 0;
    runtime.stopRequested = false;
    Launch();
}

void Process::AutoRestart()
{
    if (HasPid()) return;
    ++runtime.consecutiveRestarts;
    logger::Info("[%s] Restart %d of at most %d in a row", config.processName.c_str(), runtime.consecutiveRestarts, config.restartPolicy.maxRestarts);
    Launch();
}

void Process::Launch()
{
    logger::Debug("[%s] Starting service...", config.processName.c_str());
    if (config.useDisplay)
    {
        logger::Debug("[%s] Will use Display", config.processName.c_str());
    }
    if (!config.stdioPolicy.stdioFilePath.empty())
    {
        logger::Debug("[%s] Logs will be stored in %s", config.processName.c_str(), config.stdioPolicy.stdioFilePath.c_str());
    }

    ProcessHandle pHandle;


    SetupProcessGroupSettings();

    if (config.stdioPolicy.stdioMode == StdioMode::Pipe)
    {
        DrainStdioPipe(); // keep what the previous instance wrote
        FlushStdioLine();
        runtime.pipeFd.ConstructPipe();
    }

    // Prepared before fork: the child must not allocate.
    const bool useJournalStream = config.stdioPolicy.stdioMode == StdioMode::Inherit && IsStdoutJournal();
    const std::string journalHeader = useJournalStream ? JournalStreamHeader(config.processName) : std::string();

    pid_t pid = ::fork();
    if (pid < 0)
    {
        RemoveProcessGroup();
        runtime.pipeFd.CloseRead();
        runtime.pipeFd.CloseWrite();
        logger::Error("[%s] fork() failed with : %s", config.processName.c_str(), std::strerror(errno));
    }
    if (pid == 0) //CHILD
    {
        // Its own process group, so a stop also reaches whatever it starts.
        ::setpgid(0, 0);
        // The manager blocks its shutdown signals for its own threads; the child starts with none blocked.
        sigset_t noSignals;
        sigemptyset(&noSignals);
        ::sigprocmask(SIG_SETMASK, &noSignals, nullptr);
        if (::isatty(STDIN_FILENO))
        {
            // Outside the terminal's foreground group a read from it would stop the child (SIGTTIN).
            const int devNull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (devNull >= 0) { ::dup2(devNull, STDIN_FILENO); ::close(devNull); }
        }

        SetupWorkingDirectory();
        SetupChildGroupSettings();
        if (useJournalStream) ConnectJournalStream(journalHeader); // else it keeps the manager's output
        if (!SetupStdioSettings())
        {
            ChildError(config.processName.c_str(), "Redirecting stdout and stderr failed");
            ::_exit(EXIT_FAILURE);
        }
        if (!config.inheritParentEnvironment) ::clearenv();
        SetupDisplay();
        SetupEnvironmentVariables();
        StartChildProcess(); //exec
    }
    if (pid > 0) //PARENT
    {
        pHandle.pid = pid;
        ::setpgid(pid, pid); // as in the child: whichever runs first, the group exists before any stop
        pHandle.pgid = pid;

        if (config.stdioPolicy.stdioMode == StdioMode::Pipe)
        {
            runtime.pipeFd.CloseWrite();
            if (runtime.pipeFd.readFd >= 0)
            {
                ::fcntl(runtime.pipeFd.readFd, F_SETFL, ::fcntl(runtime.pipeFd.readFd, F_GETFL) | O_NONBLOCK);
            }
        }

        runtime.stopPhase = StopPhase::None;
        runtime.restartRequested = false;
        runtime.healthCheckSuspended = false;
        runtime.unresponsiveStop = false;

        if (config.healthPolicy.checkHealth)
        {
            GetRuntimeStatus().state = RuntimeState::Starting;
        }
        else
        {
            GetRuntimeStatus().state = RuntimeState::Running;
        }
        if (!IsUndefined(GetRuntimeStatus().startTime))
        {
            GetRuntimeStatus().restartCount++; // restarts: the first start is not one
        }
        GetRuntimeStatus().startTime = Now();
        GetRuntimeStatus().lastSeen = Now();
        GetRuntimeStatus().missedBeats = 0;
        runtime.exitStatus = ExitStatus{};
    }

    runtime.processHandle = pHandle;
}

void Process::RequestStop(const ProcessHandle& pHandle, int sig) const
{
    if (pHandle.pid <= 0) return;
    if (pHandle.pgid > 0)
    {
        ::kill(-pHandle.pgid, sig);
    }
    else
    {
        logger::Verbose("[%s] RequestStop pid %d with sig no %d", config.processName.c_str(), pHandle.pid, sig);
        ::kill(pHandle.pid, sig);
    }
}

void Process::ForceStop(const ProcessHandle& pHandle) const
{
    RequestStop(pHandle, SIGKILL);
}

// Blocking: only for shutdown and destruction. The supervision loop uses Stop() and AdvanceStop().
void Process::RequestStopAndEscalate(std::chrono::milliseconds poll)
{
    logger::Debug("[%s] RequestStopAndEscalate with polling period %ld ms and ForceKilTimeout %ld ms ", config.processName.c_str(), static_cast<long>(poll.count()), static_cast<long>(config.forceKillTimeoutMs.count()));
    if (runtime.processHandle.pid <= 0) return;

    poll = std::max(poll, std::chrono::milliseconds(1));
    Stop();
    while (IsStopping())
    {
        std::this_thread::sleep_for(poll);
        AdvanceStop();
    }
}

void Process::Stop()
{
    runtime.restartRequested = false;
    runtime.stopRequested = true; // also without a pid: it cancels a restart that is waiting for its delay
    BeginStop();
}

void Process::Restart()
{
    runtime.restartRequested = true;
    BeginStop();
}

// Stopped for missed heartbeats: the exit counts as a failure, and the restart policy restarts it.
void Process::StopUnresponsive()
{
    logger::Error("[%s] Unresponsive: %d heartbeat periods without a heartbeat, stopping it", config.processName.c_str(), runtime.runtimeHealth.missedBeats);
    runtime.restartRequested = false;
    runtime.unresponsiveStop = true;
    BeginStop();
}

void Process::BeginStop()
{
    if (!HasPid() || IsStopping()) return;

    logger::Debug("[%s] Stopping pid %d, SIGKILL after %ld ms", config.processName.c_str(), runtime.processHandle.pid, static_cast<long>(config.forceKillTimeoutMs.count()));
    runtime.stopGroup = runtime.processHandle.pgid;
    RequestStop(runtime.processHandle, SIGTERM);
    runtime.stopPhase = StopPhase::Terminating;
    runtime.stopDeadline = Now() + config.forceKillTimeoutMs;
}

// A stop ends when the process and everything left in its process group are gone.
void Process::AdvanceStop()
{
    if (!IsStopping()) return;

    ReapExit();
    if (!HasPid() && !IsGroupAlive())
    {
        FinishStop();
        return;
    }
    if (Now() < runtime.stopDeadline) return;

    if (runtime.stopPhase == StopPhase::Terminating)
    {
        logger::Debug("[%s] Graceful shutdown failed ", config.processName.c_str());
        if (runtime.stopGroup > 0) ::kill(-runtime.stopGroup, SIGKILL);
        else ForceStop(runtime.processHandle);
        if (config.createProcessGroup) TerminateProcessGroup();
        runtime.stopPhase = StopPhase::Killing;
        runtime.stopDeadline = Now() + config.forceKillTimeoutMs;
        return;
    }

    logger::Error("[%s] Force shutdown failed", config.processName.c_str());
    runtime.stopPhase = StopPhase::None;
    runtime.stopGroup = -1;
}

bool Process::IsGroupAlive() const
{
    return runtime.stopGroup > 0 && ::kill(-runtime.stopGroup, 0) == 0;
}

void Process::FinishStop()
{
    runtime.stopPhase = StopPhase::None;
    runtime.stopGroup = -1;
    if (config.createProcessGroup)
    {
        TerminateProcessGroup(); // whatever the main process left behind in its cgroup
        RemoveProcessGroup();
    }
    logger::Debug("[%s] Reaped & stop successful ", config.processName.c_str());
}

void Process::OverrideStartupArgs(std::vector<std::string> args)
{
    config.args = args;
}

void Process::SetWorkMode(WorkMode workMode)
{
    config.workMode = workMode;
}

void Process::HeartbeatReceived()
{
    logger::Debug("[%s] Heartbeat received", config.processName.c_str());

    if (!IsHealthCheckActive()) return;
    if (!HasPid()) return; // late, from an instance that has exited

    if (IsUndefined(GetRuntimeStatus().startTime))
    {
        GetRuntimeStatus().startTime = Now();
    }
    GetRuntimeStatus().lastSeen = Now();
    GetRuntimeStatus().state = RuntimeState::Running;
    GetRuntimeStatus().missedBeats = 0;
}

void Process::CheckHeartbeat()
{
    if (!HasPid()) return; // an exited process is Stopped, not Unhealthy
    if (IsGracePeriod()) return;
    if (!IsHealthCheckActive()) return;
    if (IsUndefined(GetRuntimeStatus().lastSeen)) return;
    if (config.healthPolicy.heartbeatPeriodMs.count() <= 0) return;

    // Whole heartbeat periods since the last heartbeat: 0 while it keeps up.
    GetRuntimeStatus().missedBeats = (int)((Now() - GetRuntimeStatus().lastSeen) / config.healthPolicy.heartbeatPeriodMs);
    if (GetRuntimeStatus().missedBeats > 0)
    {
        logger::Debug("[%s] Heartbeat missed", config.processName.c_str());
    }
    if (GetRuntimeStatus().missedBeats >= config.healthPolicy.missedHeartbeatThreshold)
    {
        GetRuntimeStatus().state = RuntimeState::Unhealthy;
    }
}

std::string Process::GetProcessName() const
{
    return config.processName;
}

std::vector<std::string> Process::GetDependencyList() const
{
    return config.dependsOn;
}

RuntimeState Process::GetRuntimeState() const
{
    return runtime.runtimeHealth.state;
}

uint64_t Process::GetMemoryUsageInBytes() const
{
    std::ifstream memoryFile(runtime.cgroupPath / "memory.current");
    uint64_t memUsage = 0;
    if (memoryFile >> memUsage)
    {
        return memUsage;
    }
    return 0; //error?
}

uint64_t Process::GetCpuUsageInUsec() const
{
    std::ifstream file(runtime.cgroupPath / "cpu.stat");
    std::string line;
    while (std::getline(file, line))
    {
        if (line.rfind("usage_usec", 0) == 0)
        {
            std::istringstream iss(line);
            std::string key;
            uint64_t val;
            if (iss >> key >> val)
            {
                return val;
            }
        }
    }
    return 0;
}

RuntimeHealth& Process::GetRuntimeStatus()
{
    return runtime.runtimeHealth;
}

bool Process::IsGracePeriod() const
{
    const bool isGrace = (Now() - runtime.runtimeHealth.startTime) < config.healthPolicy.gracePeriodMs;
    return isGrace;
}

bool Process::IsRunning() const
{
    return runtime.runtimeHealth.state == RuntimeState::Running;
}

bool Process::IsStarting() const
{
    return runtime.runtimeHealth.state == RuntimeState::Starting;
}

bool Process::IsStopping() const
{
    return runtime.stopPhase != StopPhase::None;
}

bool Process::IsRestartRequested() const
{
    return runtime.restartRequested;
}

bool Process::IsHealthCheckActive() const
{
    return config.healthPolicy.checkHealth && !runtime.healthCheckSuspended;
}

bool Process::ExitedWithFailure() const
{
    const auto& exitStatus = runtime.exitStatus;
    if (exitStatus.pid <= 0) return false; // no exit recorded since the last start
    if (runtime.unresponsiveStop) return true; // we stopped it for missed heartbeats

    if (!exitStatus.isTerminatedBySignal) return exitStatus.exitCode > 0;
    if (exitStatus.termSignal <= 0)
    {
        return false; // reaped elsewhere: the status is lost, which is no sign of a failure
    }

    // The signals systemd's Restart=on-failure counts as a clean stop.
    switch (exitStatus.termSignal)
    {
    case SIGHUP:
    case SIGINT:
    case SIGTERM:
    case SIGPIPE:
        return false;
    default:
        return true;
    }
}

bool Process::IsUnresponsive() const
{
    return IsHealthCheckActive() && runtime.runtimeHealth.state == RuntimeState::Unhealthy;
}

bool Process::RestartLimitReached() const
{
    return runtime.consecutiveRestarts >= config.restartPolicy.maxRestarts;
}

bool Process::IsKeptStopped() const
{
    return config.restartPolicy.restartMode == RestartMode::UnlessStopped && runtime.stopRequested;
}

// RestartDelayMs before the first restart in a row, BackoffMultiplier times longer for each one after.
std::chrono::milliseconds Process::RestartDelay() const
{
    const auto& policy = config.restartPolicy;
    const double delayMs = static_cast<double>(policy.restartDelayMs.count()) * std::pow(policy.backoffMultiplier, runtime.consecutiveRestarts);
    const double capMs = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(max_restart_delay).count());
    return std::chrono::milliseconds(static_cast<long long>(std::min(delayMs, capMs)));
}

// For a running process: restart it when it stopped answering (systemd's watchdog counts that as a
// failure too). For an exited one: per RestartMode, once the backoff delay since the exit has passed.
bool Process::NeedsRestart() const
{
    // This has turned into a spider's web.
    const auto rMode = config.restartPolicy.restartMode;
    if (rMode == RestartMode::Never) return false;
    if (IsKeptStopped())
    {
        return false;
    }
    if (RestartLimitReached()) return false;

    if (HasPid())
    {
        return !IsGracePeriod() && IsUnresponsive();
    }
    if (IsUndefined(runtime.exitTime)) return false; // never ran: NeedsStart covers it
    if (Now() - runtime.exitTime < RestartDelay()) return false;
    if (rMode == RestartMode::OnFailure)
    {
        return ExitedWithFailure();
    }
    return true;
}

bool Process::NeedsStart() const
{
    if (IsUndefined(runtime.runtimeHealth.startTime))
    {
        return true;
    }
    return false;
}

bool Process::IsWorkingManually() const
{
    return WorkMode::Manual == config.workMode;
}

bool Process::HasPid() const
{
    return runtime.processHandle.pid > 0;
}

// The main process, or one in its process group. With a cgroup, also anything under it, as its memory
// and CPU use count: a process that left the group, or outlived the main process, still belongs to it.
bool Process::IsMember(pid_t pid, pid_t pgid, const fs::path& cgroup) const
{
    if (HasPid() && (pid == runtime.processHandle.pid || (pgid > 0 && pgid == runtime.processHandle.pgid))) return true;
    if (!config.createProcessGroup || runtime.cgroupPath.empty() || cgroup.empty()) return false;

    const auto& own = runtime.cgroupPath;
    return std::mismatch(own.begin(), own.end(), cgroup.begin(), cgroup.end()).first == own.end();
}

pid_t Process::GetPid() const
{
    return runtime.processHandle.pid;
}

void Process::CheckConfig(std::vector<std::string>& errorList) const noexcept
{
    std::error_code erc;

    // These checks follow symlinks, so the same checks on the canonical path would only repeat their answers.
    if (!fs::exists(config.executablePath, erc))
    {
        errorList.push_back(GetProcessName() + " ExecutablePath does not exists");
    }
    if (fs::is_directory(config.executablePath, erc))
    {
        errorList.push_back(GetProcessName() + " ExecutablePath is a directory");
    }
    if (!fs::is_regular_file(config.executablePath, erc))
    {
        errorList.push_back(GetProcessName() + " ExecutablePath is not a regular file");
    }
    if (::access(config.executablePath.c_str(), X_OK) != 0)
    {
        errorList.push_back(GetProcessName() + " ExecutablePath access denied");
    }

    //check cwd
    // 1) Must exist
    if (!config.cwd.empty())
    {
        if (!fs::exists(config.cwd, erc))
        {
            errorList.push_back(GetProcessName() + " Cwd does not exists");
        }

        // 2) Must be a directory
        if (!fs::is_directory(config.cwd, erc))
        {
            errorList.push_back(GetProcessName() + " Cwd is not a directory");
        }
    }



    //check StdioFilePath
    if (config.stdioPolicy.stdioMode != StdioMode::File) return;
    if (config.stdioPolicy.stdioFilePath.empty())
    {
        errorList.push_back(GetProcessName() + " StdioFilePath is empty logging will fail");
        return;
    }

    // The file itself is created on the first start; its directory has to be there.
    const auto logDirectory = config.stdioPolicy.stdioFilePath.parent_path();
    if (!logDirectory.empty() && !fs::is_directory(logDirectory, erc))
    {
        errorList.push_back(GetProcessName() + " StdioFilePath directory does not exists");
    }

    return;
}

void Process::SuspendHealthCheck()
{
    runtime.healthCheckSuspended = true;
}

RuntimeHealth Process::GetRuntimeStats() const noexcept
{
    return runtime.runtimeHealth;
}


pid_t Process::ReapExit()
{
    const auto currentPid = runtime.processHandle.pid;
    ExitStatus exits;

    if (currentPid <= 0) return 0;

    while (true)
    {
        int status = 0;
        // This has been seen to hang now and then, even with WNOHANG.
        pid_t pid = ::waitpid(currentPid, &status, WNOHANG);

        if (pid < 0 && errno == ECHILD)
        {
            // No longer our child. Drop the pid, which would otherwise signal whoever reuses it;
            // the exit status stays unknown (the defaults), which ExitedWithFailure does not count.
            pid = currentPid;
        }
        else if (pid <= 0)
        {
            break;
        }
        else if (WIFEXITED(status))
        {
            exits.isTerminatedBySignal = false;
            exits.exitCode = WEXITSTATUS(status);
        }
        else if (WIFSIGNALED(status))
        {
            exits.isTerminatedBySignal = true;
            exits.termSignal = WTERMSIG(status);
        }

        exits.pid = pid;
        runtime.processHandle.pid = -1;
        runtime.processHandle.pgid = -1;
        runtime.exitStatus = exits;
        GetRuntimeStatus().state = RuntimeState::Stopped;

        runtime.exitTime = Now();
        if (runtime.exitTime - GetRuntimeStatus().startTime >= config.restartPolicy.restartResetMs)
        {
            runtime.consecutiveRestarts = 0; // it ran long enough: a new failure starts a new series
        }
        if (!exits.isTerminatedBySignal)
        {
            logger::Info("[%s] pid %d exited with code %d", config.processName.c_str(), pid, exits.exitCode);
        }
        else if (exits.termSignal > 0)
        {
            logger::Info("[%s] pid %d terminated by signal %d", config.processName.c_str(), pid, exits.termSignal);
        }
        else
        {
            logger::Info("[%s] pid %d is gone, exit status unknown", config.processName.c_str(), pid);
        }

        const auto rMode = config.restartPolicy.restartMode;
        if (!IsWorkingManually() && rMode != RestartMode::Never && !IsKeptStopped() && RestartLimitReached() &&
            (rMode != RestartMode::OnFailure || ExitedWithFailure()))
        {
            logger::Error("[%s] Not restarting: %d restarts in a row (MaxRestarts)", config.processName.c_str(), runtime.consecutiveRestarts);
        }
        return pid;

    }
    return -1;
}

DetailedHealthReport Process::GetHealthReport() const
{
    // Sent as raw bytes: zero the padding too, and keep the name NUL-terminated.
    DetailedHealthReport healthReport;
    std::memset(static_cast<void*>(&healthReport), 0, sizeof(healthReport));
    const size_t nameLength = std::min(config.processName.size(), sizeof(healthReport.processName) - 1);
    std::memcpy(healthReport.processName, config.processName.data(), nameLength);

    const auto& health = runtime.runtimeHealth;
    healthReport.runtimeHealth.state = health.state;
    healthReport.runtimeHealth.startTime = health.startTime;
    healthReport.runtimeHealth.lastSeen = health.lastSeen;
    healthReport.runtimeHealth.missedBeats = health.missedBeats;
    healthReport.runtimeHealth.restartCount = health.restartCount;
    healthReport.pid = GetPid();
    healthReport.memoryUsageInBytes = GetMemoryUsageInBytes();
    healthReport.cpuUsageInUsec = GetCpuUsageInUsec();
    healthReport.snapshotTime = Now();
    return healthReport;
}

void Process::DrainStdioPipe()
{
    char buffer[4096];
    for (int reads = 0; runtime.pipeFd.readFd >= 0 && reads < max_stdio_reads; ++reads)
    {
        const ssize_t count = ::read(runtime.pipeFd.readFd, buffer, sizeof(buffer));
        if (count > 0)
        {
            AppendStdioOutput(std::string_view(buffer, static_cast<size_t>(count)));
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;

        // End of file: every writer is gone.
        FlushStdioLine();
        runtime.pipeFd.CloseRead();
    }
}

void Process::AppendStdioOutput(std::string_view output)
{
    auto& line = runtime.stdioLine;
    line.append(output.data(), output.size());

    size_t start = 0;
    for (size_t newline = line.find('\n'); newline != std::string::npos; newline = line.find('\n', start))
    {
        const std::string text = line.substr(start, newline - start);
        logger::Info("[%s] %s", config.processName.c_str(), text.c_str());
        start = newline + 1;
    }
    line.erase(0, start);

    if (line.size() >= max_stdio_line) FlushStdioLine();
}

void Process::FlushStdioLine()
{
    if (runtime.stdioLine.empty()) return;
    logger::Info("[%s] %s", config.processName.c_str(), runtime.stdioLine.c_str());
    runtime.stdioLine.clear();
}

bool Process::SetupStdioPipe() const
{
    const auto& pipe = runtime.pipeFd;
    ::close(pipe.readFd);

    // Redirect stdout and stderr to the write end of the pipe
    if (::dup2(pipe.writeFd, STDOUT_FILENO) == -1 || ::dup2(pipe.writeFd, STDERR_FILENO) == -1)
    {
        return false;
    }

    // Close the original write end fd
    ::close(pipe.writeFd);
    return true;
}
bool Process::SetupStdioFile(const char* path) const
{
    const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (config.stdioPolicy.stdioAppend ? O_APPEND : O_TRUNC);
    const int fd = ::open(path, flags, 0644);
    if (fd < 0) return false;

    // Redirect stdout and stderr to the file
    const bool redirected = ::dup2(fd, STDOUT_FILENO) != -1 && ::dup2(fd, STDERR_FILENO) != -1;
    ::close(fd);
    return redirected;
}
std::vector<char*> Process::SetupExecParams() const
{
    std::vector<char*> argv_c;
    argv_c.push_back(const_cast<char*>(config.executablePath.c_str()));
    for (const auto& arg : config.args)
    {
        argv_c.push_back(const_cast<char*>(arg.data()));
    }
    argv_c.push_back(nullptr);

    return argv_c;
}
bool Process::SetupWorkingDirectory() const
{
    const auto& cwd = config.cwd;

    if (!cwd.empty())
    {
        if (::chdir(cwd.c_str()) != 0)
        {
            ChildError(config.processName.c_str(), "chdir failed"); // in the child: no logger, it takes a lock
        }
    }

    return true;
}
bool Process::SetupEnvironmentVariables()
{
    for (auto& arg : config.environmentVariables)
    {
        ::putenv(arg.data());
    }
    return true;
}
bool Process::SetupDisplay() const
{
    if (!config.useDisplay) return true;

    if (setenv("DISPLAY", config.display.c_str(), 1) != 0)
    {
        ChildError(config.processName.c_str(), "Setting DISPLAY failed");
    }
    if (!config.xAuthority.empty() && setenv("XAUTHORITY", config.xAuthority.c_str(), 1) != 0)
    {
        ChildError(config.processName.c_str(), "Setting XAUTHORITY failed");
    }
    return true;
}
bool Process::SetupStdioSettings() const
{
    const auto& stdio = config.stdioPolicy;

    switch (stdio.stdioMode)
    {
    case StdioMode::Inherit:
    {
        return true;
    }
    case StdioMode::Pipe:
    {
        return SetupStdioPipe();
    }
    case StdioMode::NullDevice:
    {
        return SetupStdioFile("/dev/null");
    }
    case StdioMode::File:
    {
        return SetupStdioFile(stdio.stdioFilePath.c_str());
    }
    default:
        return true; // an unknown mode keeps the manager's output, as Inherit does
    }

    return false;
}

bool Process::SetupChildGroupSettings() const
{
    if (!config.createProcessGroup) return true;

    std::string procs_file = (runtime.cgroupPath / "cgroup.procs").string();
    std::string pid_str = std::to_string(getpid());

    int fd = ::open(procs_file.c_str(), O_WRONLY);
    if (fd < 0 || write(fd, pid_str.c_str(), pid_str.length()) < 0)
    {
        ChildError(config.processName.c_str(), "Joining the service's cgroup failed"); // in the child: no logger
        _exit(EXIT_FAILURE); // Immediate exit, bypass C++ destructors
    }
    close(fd);
    return true;
}

bool Process::SetupProcessGroupSettings()
{
    if (!config.createProcessGroup) return true;

    fs::path managerRoot = ProcessGroupBuilder::GetInstance().GetParentGroupPath();
    runtime.cgroupPath = managerRoot / ("task_" + config.processName);

    std::error_code ec;
    if (!fs::create_directories(runtime.cgroupPath, ec) && ec)
    {
        logger::Error("[%s] Failed to create cgroup directory:  %s ", config.processName.c_str(), ec.message().c_str());
        logger::Error("[%s] Starting directly... ", config.processName.c_str());
        config.createProcessGroup = false;
        return false;
    }
    return true;
}

bool Process::TerminateProcessGroup() const
{
    if (!config.createProcessGroup) return true;

    fs::path kill_file = runtime.cgroupPath / "cgroup.kill";
    std::ofstream kill_stream(kill_file);
    if (kill_stream.is_open())
    {
        kill_stream << "1" << std::flush;
        kill_stream.close();
    }
    else
    {
        return false;
    }
    return true;

}

bool Process::RemoveProcessGroup() const
{
    if (!config.createProcessGroup) return true;

    std::error_code ec;
    if (!fs::remove(runtime.cgroupPath, ec) && ec)
    {
        logger::Error("[%s] Failed to delete cgroup directory:  %s", config.processName.c_str(), std::strerror(errno));
        return false;
    }
    return true;
}

bool Process::StartChildProcess() const
{
    ::execvp(config.executablePath.c_str(), SetupExecParams().data());
    // exec failed. _exit: the parent's atexit handlers and static destructors must not run in this copy.
    ChildError(config.processName.c_str(), "exec failed");
    ::_exit(127);
}

} // namespace process_manager
