#pragma once 
#include <IProcessManager.h>
#include <legacy/common/logger/ILogManager.h> 
//#include <legacy/common/logger/ILogAdapter.h> //meliksahs todo 
#include <legacy/logger/log_adapter/ConsoleLogAdapter.h> 
#include <legacy/logger/log_adapter/FileLogAdapter.h> 
#include <legacy/timer/StaticTimerManager.h>
#include <legacy/timer/multi/StaticMultiUseTimer.h>
#include <legacy/common/type/IObject.h>
#include <ProcessManagerCommands.h>

#include <memory>
#include <string>
#include <vector>

namespace process_manager {

	class ProcessManagerApp : public legacy::type::IObject {
	public:
		ProcessManagerApp() = default;
		~ProcessManagerApp() = default;

		legacy::enums::RetVal StartUp(const std::string& configPath = ""); // empty: the installed config
		legacy::enums::RetVal InitializeProcessManager();
		legacy::enums::RetVal InitializeLogManager();
		legacy::enums::RetVal InitializeTimer();

		legacy::enums::RetVal TearDown();
		legacy::enums::RetVal Run();
		legacy::enums::RetVal ShutdownSignalReceived(int signum);
		
		legacy::object::ObjectType GetType() const override;


		// allowFallback: try the share directory's copy when configPath cannot be opened.
		static legacy::enums::RetVal InitializeConfigManager(const char* configPath, bool allowFallback);

		static bool ValidateConfig(std::string config); // true when no problem was found
		static void PrintDependencyGraph(std::string config);

		static void PrintProcessStatus();
		static bool SendStartService(std::string serviceName);
		static bool SendDisableService(std::string serviceName);
		static bool SendKillService(std::string serviceName);
		static bool SendRestartService(std::string serviceName);
	private:
		static void PrintErrors(const std::vector<std::string>& errList);
		static bool SendCommandMessage(ProcessManagerCommandEnum command, const std::string& serviceName);

		std::unique_ptr<IProcessManager> processManager;
		legacy::logger::ILogManager* logManager;
		legacy::logger::ConsoleLogAdapter consoleLogAdapter;
		legacy::logger::FileLogAdapter fileLogAdapter;
		legacy::timer::StaticMultiUseTimer<128> multiUseTimer;
		legacy::timer::StaticTimerManager<32> timerManager;


	};

}