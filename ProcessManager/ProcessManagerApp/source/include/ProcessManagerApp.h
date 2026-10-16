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
#include <vector>

namespace process_manager {

	class ProcessManagerApp : public legacy::type::IObject {
	public:
		ProcessManagerApp() = default;
		~ProcessManagerApp() = default;

		legacy::enums::RetVal StartUp();
		legacy::enums::RetVal InitializeProcessManager();
		legacy::enums::RetVal InitializeLogManager();
		legacy::enums::RetVal InitializeTimer();

		legacy::enums::RetVal TearDown();
		legacy::enums::RetVal Run();
		legacy::enums::RetVal ShutdownSignalReceived(int signum);
		
		legacy::object::ObjectType GetType() const override;


		static legacy::enums::RetVal InitializeConfigManager(const char* configPath);

		static void ValidateConfig(std::string config);
		static void PrintDependencyGraph(std::string config);

		static void PrintProcessStatus();
		static void SendStartService(std::string serviceName);
		static void SendDisableService(std::string serviceName);
		static void SendKillService(std::string serviceName);
		static void SendRestartService(std::string serviceName);
	private:
		static void PrintErrors(const std::vector<std::string>& errList);
		static void SendCommandMessage(ProcessManagerCommandEnum command, const std::string& serviceName);

		std::unique_ptr<IProcessManager> processManager;
		legacy::logger::ILogManager* logManager;
		legacy::logger::ConsoleLogAdapter consoleLogAdapter;
		legacy::logger::FileLogAdapter fileLogAdapter;
		legacy::timer::StaticMultiUseTimer<128> multiUseTimer;
		legacy::timer::StaticTimerManager<32> timerManager;


	};

}