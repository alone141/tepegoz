#pragma once
#include <string>
#include <filesystem>
#include <vector>

#include <legacy/common/type/Arguments.h>
#include <StdioConfigStruct.h>
#include <RestartPolicyStruct.h>
#include <HealthPolicyStruct.h>
#include <ExitStatusStruct.h>
#include <ProcessHandle.h>
#include <PipeStruct.h>
#include <RuntimeHealthStruct.h>
#include <WorkModeEnum.h>
namespace process_manager
{

	struct ProcessConfigParams : public legacy::type::Arguments
	{

		int UpdateArgs(const char* sectionOrder);

		std::string ProcessName;
		std::filesystem::path ExecutablePath;
		std::vector<std::string> Args;

		std::filesystem::path Cwd;
		bool InheritParentEnviroment = true;
		bool UseDisplay = false;

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
		WorkModeEnum WorkMode;

		//std::unordered_map<std::string, std::string> metadata;


	};
	struct ProcessRuntimeParams
	{
		ProcessHandleStruct processHandle{};
		ExitStatusStruct exitStatus{};
		PipeStruct pipeFd{};
		RuntimeHealthStruct runtimeHealth{};
		std::filesystem::path cgroup_path;
	};
}