#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <filesystem>
#include <sys/types.h>
#include <vector>

#include <StdioConfigStruct.h>
#include <RestartPolicyStruct.h>
#include <HealthPolicyStruct.h>
#include <ExitStatusStruct.h>
#include <ProcessHandle.h>
#include <PipeStruct.h>
#include <RuntimeHealthStruct.h>
#include <StopPhaseEnum.h>
#include <WorkModeEnum.h>
namespace process_manager
{

	struct ProcessConfigParams
	{

		// 1 when loaded, -1 when ProcessName or ExecutablePath is missing, -2 when ProcessName is reserved.
		int UpdateArgs(config_section::ConfigSection& section);
		static bool IsReservedName(const std::string& name);

		std::string ProcessName;
		std::filesystem::path ExecutablePath;
		std::vector<std::string> Args;

		std::filesystem::path Cwd;
		bool InheritParentEnviroment = true;
		bool UseDisplay = false;
		std::string Display = ":1";
		std::string XAuthority = "/home/user/.Xauthority";

		//meliksahs TODO
		//std::unordered_map<std::string, std::string> env_overrides;
		std::vector<std::string> EnviromentVariables;


		std::chrono::milliseconds ForceKillTimeoutMs{ 10'000 };

		//meliksahs TODO
		//int StopSignal = SIGTERM;
		bool CreateProcessGroup = true;
		//bool Detach = false;

		std::vector<std::string> DependsOn;

		//meliksahs TODO
		//StdioPolicyStruct stdin{};
		//StdioPolicyStruct stderr{};
		StdioPolicyStruct StdioPolicy{};
		RestartPolicyStruct RestartPolicy{};
		HealthPolicyStruct HealthPolicy{};
		WorkModeEnum WorkMode = WorkModeEnum::Manual;

		//std::unordered_map<std::string, std::string> metadata;


	};
	struct ProcessRuntimeParams
	{
		ProcessHandleStruct processHandle{};
		ExitStatusStruct exitStatus{};
		PipeStruct pipeFd{};
		RuntimeHealthStruct runtimeHealth{};
		std::filesystem::path cgroup_path;

		StopPhaseEnum stopPhase = StopPhaseEnum::None;
		std::chrono::high_resolution_clock::time_point stopDeadline{};
		pid_t stopGroup = -1; // process group being stopped; outlives the reaped leader
		bool restartRequested = false;
		bool healthCheckSuspended = false;
		bool unresponsiveStop = false;
		std::string stdioLine;

		std::chrono::high_resolution_clock::time_point exitTime{};
		int consecutiveRestarts = 0;
	};
}