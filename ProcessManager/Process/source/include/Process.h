#pragma once 
#include <ProcessConfigParamsStruct.h>
#include <legacy/common/type/IObject.h>

namespace process_manager
{
	class Process : public legacy::type::IObject
	{
	public:
		~Process();
		Process() = default;

		void StartProcess();

		int UpdateArgs(const char* sectionOrder);
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
		bool NeedsRestart() const;
		bool NeedsStart() const;
		bool IsWorkingManually() const;

		void CheckConfig(std::vector<std::string>& errorList) const noexcept;
		void CheckHeartbeat();
		void DisableHealthCheck();
		void HeartbeatReceived();
		void RequestStopAndEscalate(std::chrono::milliseconds poll = std::chrono::milliseconds(50));

		void OverrideStartupArgs(std::vector<std::string> args);
		void SetWorkMode(WorkModeEnum workMode);
		legacy::object::ObjectType GetType() const override;

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
		std::vector<char*> SetupEnviromentParams() const;


		ProcessRuntimeParams runtime;
		ProcessConfigParams config;
	};


}