#pragma once
#include <ProcessConfigParamsStruct.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace config_section
{
    class ConfigSection;
}

namespace process_manager
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
        process_manager::DetailedHealthReportStruct GetHealthReport() const;
        process_manager::RuntimeHealthStruct GetRuntimeStats() const noexcept;
        process_manager::RuntimeStateEnum GetRuntimeState() const;
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
        void SetWorkMode(WorkModeEnum workMode);

    private:
        static auto Now()
        {
            return std::chrono::high_resolution_clock::now();
        }
        static bool IsUndefined(const std::chrono::high_resolution_clock::time_point& timepoint)
        {
            return timepoint == std::chrono::high_resolution_clock::time_point{};
        }

        RuntimeHealthStruct& GetRuntimeStatus();
        bool ExitedWithFailure() const;
        bool IsHealthCheckActive() const;
        bool IsUnresponsive() const;
        bool RestartLimitReached() const;
        std::chrono::milliseconds RestartDelay() const;
        bool IsGroupAlive() const;

        void Launch();
        void BeginStop();
        void FinishStop();
        void AppendStdioOutput(std::string_view output);
        void FlushStdioLine();

        bool SetupStdioPipe() const;
        bool SetupStdioFile(const char* path) const;
        bool SetupWorkingDirectory() const;
        bool SetupEnviromentVariables();
        bool SetupDisplay() const;
        bool SetupStdioSettings() const;
        bool StartChildProcess() const;

        bool SetupChildGroupSettings() const;
        bool SetupProcessGroupSettings();
        bool TerminateProcessGroup() const;
        bool RemoveProcessGroup() const;

        void RequestStop(const ProcessHandleStruct& pHandle, int sig) const;
        void ForceStop(const ProcessHandleStruct& pHandle) const;
        std::vector<char*> SetupExecParams() const;


        ProcessRuntimeParams runtime;
        ProcessConfigParams config;
    };


}