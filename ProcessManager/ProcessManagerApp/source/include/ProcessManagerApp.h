#pragma once
#include <IProcessManager.h>
#include <ProcessManagerCommands.h>

#include <memory>
#include <string>
#include <vector>

namespace config_section
{
	struct ConfigValue;
}

namespace process_manager {

	class ProcessManagerApp {
	public:
		ProcessManagerApp() = default;
		~ProcessManagerApp() = default;

		bool StartUp(const std::string& configPath = ""); // empty: the installed config; false: cannot run

		void TearDown();
		void Run();
		void ShutdownSignalReceived(int signum);

		// allowFallback: try the share directory's copy when configPath cannot be read.
		static bool LoadConfig(const std::string& configPath, bool allowFallback, config_section::ConfigValue& document);

		static bool ValidateConfig(std::string config); // true when no problem was found
		static void PrintDependencyGraph(std::string config);

		static void PrintProcessStatus();
		static bool SendStartService(std::string serviceName);
		static bool SendDisableService(std::string serviceName);
		static bool SendKillService(std::string serviceName);
		static bool SendRestartService(std::string serviceName);
	private:
		static void InitializeLogger();
		bool InitializeProcessManager(const config_section::ConfigValue& document);
		static void PrintErrors(const std::vector<std::string>& errList);
		static bool SendCommandMessage(ProcessManagerCommandEnum command, const std::string& serviceName);

		std::unique_ptr<IProcessManager> processManager;
	};

}