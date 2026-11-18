#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <filesystem>
#include <sys/types.h>
#include <vector>

#include "StdioPolicy.hpp"
#include "RestartPolicy.hpp"
#include "HealthPolicy.hpp"
#include "ExitStatus.hpp"
#include "ProcessHandle.hpp"
#include "Pipe.hpp"
#include "RuntimeHealth.hpp"
#include "StopPhase.hpp"
#include "WorkMode.hpp"
namespace tepegoz
{

struct ProcessConfigParams
{

    // 1 when loaded, -1 when ProcessName or ExecutablePath is missing, -2 when ProcessName is reserved,
    // -3 when a renamed key is set in both spellings (read in full, so the section's problems are complete).
    int UpdateArgs(config_section::ConfigSection& section);
    static bool IsReservedName(const std::string& name);

    std::string processName;
    std::filesystem::path executablePath;
    std::vector<std::string> args;

    std::filesystem::path cwd;
    bool inheritParentEnvironment = true;
    bool useDisplay = false;
    std::string display = ":1";
    std::string xAuthority = "/home/user/.Xauthority";

    std::vector<std::string> environmentVariables;


    std::chrono::milliseconds forceKillTimeoutMs{ 10'000 };

    bool createProcessGroup = true;
    //bool Detach = false;

    std::vector<std::string> dependsOn;

    StdioPolicy stdioPolicy{};
    RestartPolicy restartPolicy{};
    HealthPolicy healthPolicy{};
    WorkMode workMode = WorkMode::Manual;

    //std::unordered_map<std::string, std::string> metadata;


};
struct ProcessRuntimeParams
{
    ProcessHandle processHandle{};
    ExitStatus exitStatus{};
    Pipe pipeFd{};
    RuntimeHealth runtimeHealth{};
    std::filesystem::path cgroupPath;

    StopPhase stopPhase = StopPhase::None;
    std::chrono::high_resolution_clock::time_point stopDeadline{};
    pid_t stopGroup = -1; // process group being stopped; outlives the reaped leader
    bool restartRequested = false;
    bool stopRequested = false; // Stop() since the last StartProcess(): UnlessStopped keeps it stopped
    bool healthCheckSuspended = false;
    bool unresponsiveStop = false;
    std::string stdioLine;

    std::chrono::high_resolution_clock::time_point exitTime{};
    int consecutiveRestarts = 0;
};

} // namespace tepegoz
