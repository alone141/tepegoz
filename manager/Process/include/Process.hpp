#pragma once
#include "ProcessConfigParams.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace config_section
{

class ConfigSection;

} // namespace config_section

namespace tepegoz
{

// Owns its child process: a destroyed or overwritten Process stops the child it still holds,
// so copies are forbidden and a move hands the child over.
class Process
{
public:
    ~Process();
    Process() = default;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;

    void StartProcess(); // a start the restart policy did not ask for: re-arms MaxRestarts
    void AutoRestart();

    int UpdateArgs(config_section::ConfigSection& section); // ProcessConfigParams::UpdateArgs codes
    pid_t ReapExit();

    pid_t GetPid() const;
    std::string GetProcessName() const;
    std::vector<std::string> GetDependencyList() const;
    DetailedHealthReport GetHealthReport() const;
    RuntimeHealth GetRuntimeStats() const noexcept;
    RuntimeState GetRuntimeState() const;
    uint64_t GetMemoryUsageInBytes() const;
    uint64_t GetCpuUsageInUsec() const;
    bool HasPid() const;
    bool IsGracePeriod() const;
    bool IsRunning() const;
    bool IsStarting() const;
    bool IsStopping() const;
    bool IsRestartRequested() const;
    bool NeedsRestart() const;
    bool NeedsStart() const;
    bool IsWorkingManually() const;
    // A process with this pid, process group and cgroup belongs to the service.
    bool IsMember(pid_t pid, pid_t pgid, const std::filesystem::path& cgroup) const;

    void CheckConfig(std::vector<std::string>& errorList) const noexcept;
    void CheckHeartbeat();
    void SuspendHealthCheck(); // until the next start
    void HeartbeatReceived();
    void Stop();
    void Restart();
    void StopUnresponsive();
    void AdvanceStop();
    void DrainStdioPipe();
    void RequestStopAndEscalate(std::chrono::milliseconds poll = std::chrono::milliseconds(50));

    void OverrideStartupArgs(std::vector<std::string> args);
    void SetWorkMode(WorkMode workMode);

    // A reload's new definition of the service, from a Process built from it: it waits for the next start and replaces
    // the definition then. Until it does, the health report marks the service. Nothing is started or stopped here.
    void DeferDefinition(const Process& next);
    bool HasDeferredDefinition() const;
    // Takes the waiting definition now, work mode included. For a service that is not running: a start command
    // calls it before it chooses the work mode.
    void ApplyDeferredDefinition();

private:
    // Deadlines and waits are measured on the steady clock, which setting the machine's time does not move. Only
    // the times in the health report are wall-clock times.
    static std::chrono::steady_clock::time_point Now()
    {
        return std::chrono::steady_clock::now();
    }
    static bool IsUndefined(const std::chrono::steady_clock::time_point& timepoint)
    {
        return timepoint == std::chrono::steady_clock::time_point{};
    }

    RuntimeHealth& GetRuntimeStatus();
    bool ExitedWithFailure() const;
    bool IsHealthCheckActive() const;
    bool IsUnresponsive() const;
    bool RestartLimitReached() const;
    bool IsKeptStopped() const;
    std::chrono::milliseconds RestartDelay() const;
    bool IsGroupAlive() const;

    void Launch();
    void BeginStop();
    void FinishStop();
    void StopLeftovers(pid_t group);
    void AppendStdioOutput(std::string_view output);
    void FlushStdioLine();

    bool SetupStdioPipe() const;
    bool SetupStdioFile(const char* path) const;
    bool SetupWorkingDirectory() const;
    bool SetupEnvironmentVariables();
    bool SetupDisplay() const;
    bool SetupStdioSettings() const;
    bool StartChildProcess() const;

    bool SetupChildGroupSettings() const;
    bool SetupProcessGroupSettings();
    bool TerminateProcessGroup() const;
    bool RemoveProcessGroup() const;

    void RequestStop(const ProcessHandle& pHandle, int sig) const;
    void ForceStop(const ProcessHandle& pHandle) const;
    std::vector<char*> SetupExecParams() const;


    ProcessRuntimeParams runtime;
    ProcessConfigParams config;
    std::optional<ProcessConfigParams> deferredConfig; // DeferDefinition()
};

} // namespace tepegoz
