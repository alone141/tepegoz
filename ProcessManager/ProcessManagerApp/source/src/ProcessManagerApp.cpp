#include <legacy/conf/ConfigurationManager.h>
#include <legacy/conf/ConfigurationManagerStartParam.h>
#include <legacy/logger/log_manager/TLogManager.hpp>
#include <legacy/logger/log_manager/LogManagerParameter.h>
#include <legacy/logger/log_adapter/FileLogAdapterArg.h>
#include <legacy/logger/log_adapter/ConsoleLogAdapter.h>
#include <legacy/timer/multi/MultiUseTimerStartupArgs.h>
#include <legacy/oslib/file/FileManager.h>
#include <legacy/oslib/file/File.h> 
#include <ProcessManagerApp.h>
#include <ProcessManager.h>
#include <ConfigHelper.h>
#include <iomanip>
#include <unordered_map>
#include <thread>
#include <zmq.hpp>
using namespace legacy;
using namespace legacy::enums;
using namespace legacy::oslib;
using namespace legacy::conf;
using namespace legacy::mem;
using namespace legacy::logger;
using namespace process_manager;
using namespace std;


RetVal ProcessManagerApp::StartUp(const std::string& configPath)
{
	auto retVal = InitializeTimer();
	retVal = InitializeLogManager();

	LOGF_INFO("PROCESMANAGER VERSION: %s", PROCESSMANAGER_VERSION_FULL);


	// A file named with --config is used as given; only the installed one may fall back to the share directory.
	const bool givenPath = !configPath.empty();
	retVal = InitializeConfigManager(givenPath ? configPath.c_str() : PROCESS_MANAGER_CONFIG_FILE, !givenPath);
	if (retVal != RetVal::Success) return retVal;
	retVal = InitializeProcessManager();

	if (retVal != RetVal::Success)
	{
		LOGF_ERROR("Process Manager failed to start");
	}

	return retVal;
}

RetVal ProcessManagerApp::Run()
{
	if (processManager) processManager->Run();
	LOGF_DEBUG("Exiting ProcessManager Thread");
	return RetVal::Success;
}

// Called from main's signal thread: only asks the supervision thread to stop; Run() returns after.
legacy::enums::RetVal process_manager::ProcessManagerApp::ShutdownSignalReceived(int signum)
{
	LOGF_WARNING(" ==== SHUTDOWN SIGNAL RECEIVED signum: %d  ==== ", signum)
	if (processManager) processManager->RequestStop();
	return RetVal::Success;
}

object::ObjectType process_manager::ProcessManagerApp::GetType() const
{
	return legacy::object::ProcessManagerApp;
}

RetVal ProcessManagerApp::InitializeConfigManager(const char* configPath, bool allowFallback)
{
	static ConfigurationManager configManager;
	auto file = legacy::mem::Alloc<File>::MakeUnique(configPath);
	if (!file)
	{
		return RetVal::NullPointer;
	}
	if (file->Open() != RetVal::Success)
	{
		if (!allowFallback)
		{
			LOGF(object::ProcessManagerApp, LogLevel::Error, "Config file %s could not be opened", configPath);
			cerr << "Config file " << configPath << " could not be opened" << endl;
			return RetVal::FileNotOpened;
		}
		LOGF(object::ProcessManagerApp, LogLevel::Warning, "Config file could not found at %s", configPath);
		LOGF(object::ProcessManagerApp, LogLevel::Warning, "Searching config file at %s", PROCESS_MANAGER_SHARE_DIR"ProcessManagerConfig.ini");
		file = legacy::mem::Alloc<File>::MakeUnique(PROCESS_MANAGER_SHARE_DIR"ProcessManagerConfig.ini");

	}
	file->Close();
	ConfigurationManagerStartParameter params{ move(file) };
	auto ret = reinterpret_cast<ConfigurationManager*>(&configManager)->StartUp(&params);

	if (!ret)
	{
		LOGF(object::ProcessManagerApp, LogLevel::Error, "ConfigurationManager Config file not found Use --help flag for more information");
		LOGF(object::ProcessManagerApp, LogLevel::Error, "Use --help flag for more information");
		return RetVal::FileNotOpened;
	}

	ConfigHelper::Initialize(&configManager);
	LOGF(object::ProcessManagerApp, LogLevel::Info, "ConfigurationManager Initiliazed");

	return RetVal::Success;
}

RetVal ProcessManagerApp::InitializeProcessManager()
{
	auto config = ProcessManager::GenerateConfig();
	try
	{
		processManager = make_unique<ProcessManager>(move(config));
	}
	catch (const zmq::error_t& e)
	{
		// For example another manager already holds the health ports.
		LOGF_ERROR("Process manager sockets could not be opened: %s", e.what());
		return RetVal::UnitStartUpFailure;
	}
	auto cycleDetected = !processManager->StartUp();
	return cycleDetected ? RetVal::UnitStartUpFailure : RetVal::Success;
}

RetVal ProcessManagerApp::InitializeTimer()
{
	timer::MultiUseTimerStartupArgs params(&timerManager, 100);

	auto ret = multiUseTimer.StartUp(&params);

	return ret;
}
RetVal ProcessManagerApp::InitializeLogManager()
{
	auto StartTimeMs(Clock::GetMsec());
	logManager = TLogManager<8, 1024>::GetInstance();
	auto args = LogManagerParameter(&multiUseTimer);

	auto ret = logManager->StartUp(&args);
	logManager->Suspend();

	bool fileEnable = true;

	if (fileEnable)
	{
		// FileLogAdapter::SetStartTime(AppConfiguration::GetObject().StartTimeMs);

		int32_t logLvl = LogLevel::Debug;
		auto logDirectory = PROCESS_MANAGER_LOG_DIR;

		if (!oslib::FileManager::DirExists(logDirectory))
		{
			if (!oslib::FileManager::CreateDir(logDirectory, true))
			{
				ERROR_PRINT("File log directory creation error: \"%s\"", logDirectory);
				////INFO_PRINT("RunTime %ld will not take file log!!!", AppConfiguration::GetObject().StartTimeMs);
				fileEnable = false;
			}
		}

		if (fileEnable)
		{
			char logFile[256];
			time_t tm_ = (time_t)(StartTimeMs / 1000);
			tm* now = localtime(&tm_);

			snprintf(logFile, 256, "%04d_%02d_%02d_%02d_%02d_%02d", now->tm_year + 1900, now->tm_mon + 1, now->tm_mday, now->tm_hour, now->tm_min,
				now->tm_sec);

			oslib::FilePath logPath = oslib::FilePath(logDirectory) / logFile;

			int32_t chunkFileSizePerLog = 100; /*AppConfiguration::GetObject().LoggerArgs.FileChunkSize*/;
			FileLogAdapterArg arg{ logPath, &multiUseTimer, "PMLog", chunkFileSizePerLog };

			if (!logManager->RegisterLogAdapter(&fileLogAdapter, &arg, logLvl))
			{
				DEBUG_PRINT("FileLogManager Register Error\n");
			}
			else
			{
				//INFO_PRINT("LogManager File Path=%s Registered\n", logPath.CStr());
			}
		}
	}

	bool consoleEnable = true;

	if (consoleEnable)
	{
		// ConsoleLogAdapter::SetStartTime(AppConfiguration::GetObject().StartTimeMs);

		int32_t logLvl = LogLevel::Debug;

		LogAdapterArg arg;
		if (!logManager->RegisterLogAdapter(&consoleLogAdapter, &arg, logLvl))
		{
			DEBUG_PRINT("ConsoleLogManager Register Error\n");
		}
		else
		{
			//INFO_PRINT("LogManager Console Registered\n");
		}
	}

	return logManager->Resume();
}
RetVal ProcessManagerApp::TearDown()
{
	if (processManager) processManager->TearDown();

	usleep(500);
	logManager->Suspend();
	logManager->ShutDown();
	return RetVal::Success;
}

bool ProcessManagerApp::ValidateConfig(string config)
{
	vector<string> err;
	auto retVal = InitializeConfigManager(config.c_str(), false);
	if (retVal != RetVal::Success) { return false; }
	auto params = ProcessManager::GenerateConfig(&err);
	ProcessManager::SortByDependencies(err, params->processList); // duplicates, missing dependencies, cycles
	for (const auto& process : params->processList)
	{
		process.CheckConfig(err);
	}
	PrintErrors(err);
	return err.empty();
}

void ProcessManagerApp::PrintDependencyGraph(string config)
{
	vector<string> err;
	auto retVal = InitializeConfigManager(config.c_str(), false);
	if (retVal != RetVal::Success) return;

	auto params = ProcessManager::GenerateConfig(&err);
	ProcessManager::SortByDependencies(err, params->processList);

	PrintErrors(err);
	cout << "Start Order:" << endl;

	for (const auto& process : params->processList)
	{
		cout << process.GetProcessName() << endl;
	}
}

void ProcessManagerApp::PrintErrors(const vector<string>& errList)
{
	if (!errList.empty())
	{
		cout << "================================================" << endl;
		cout << "PLEASE FIX MENTIONED WARNINGS IN THE CONFIG FILE" << endl;
		for (const auto& error : errList)
		{
			cout << error << endl;
		}
		cout << "================================================" << endl;

	}
	else
	{
		cout << "NO ERRORS WERE DETECTED" << endl;
	}
}

void ProcessManagerApp::PrintProcessStatus()
{
	zmq::context_t context(1);
	zmq::socket_t sub(context, zmq::socket_type::sub);
	sub.set(zmq::sockopt::conflate, 1);
	sub.connect(comm::HEALTH_LISTEN_PORT_DETAILED);
	sub.set(zmq::sockopt::subscribe, "");

	// fixed column widths
	const int wName = 13;
	const int wPid = 13;
	const int wStatus = 17;
	const int wMissed = 15;
	const int wStart = 14;
	const int wCpu = 16;
	const int wMem = 21;
	constexpr int totalWidth = wName +wPid + wStatus + wMissed + wStart + wCpu + wMem;

	std::unordered_map<pid_t, DetailedHealthReportStruct> processHistory;
	while(true)
	{
		zmq::message_t msg;

		(void)sub.recv(msg, zmq::recv_flags::none);
		auto* p = static_cast<const DetailedHealthReportStruct*>(msg.data());
		std::vector<DetailedHealthReportStruct> payload(p, p + msg.size() / sizeof(DetailedHealthReportStruct));

		// ANSI escape sequence: 
		// \033[2J clears the screen
		// \033[1;1H moves the cursor to row 1, column 1
		std::cout << "\033[2J\033[1;1H";

		std::cout << std::left
			<< std::setw(wName) << "ServiceName"
			<< std::setw(wPid) << "Process Id"
			<< std::setw(wStatus) << "Process Status"
			<< std::setw(wMissed) << "Missed Beats"
			<< std::setw(wStart) << "Restarts"
			<< std::setw(wCpu) << "CPU Usage(us)"
			<< std::setw(wMem) << "Memory Usage(MB)" << '\n';

		std::cout << std::setfill('-') << std::setw(totalWidth) << "" << '\n';
		std::cout << std::setfill(' ');

		for (const auto& process : payload)
		{
			double memoryInMB = 0;
			double cpuPercentage = 0;
			auto const isAlive = process.runtimeHealth.state == RuntimeStateEnum::Running ||
				process.runtimeHealth.state == RuntimeStateEnum::Starting;
			if (isAlive)
			{
				memoryInMB = process.memoryUsageInBytes / (double)(1024 * 1024);
			}

			auto it = processHistory.find(process.pid);
			if (it != processHistory.end())
			{
				uint64_t deltaCpuUs = process.cpuUsageInUsec - it->second.cpuUsageInUsec;
				uint64_t deltaTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(process.snapshotTime - it->second.snapshotTime).count();

				if (deltaTimeUs > std::chrono::milliseconds(300).count())
				{
					if (isAlive)
					{
						cpuPercentage = (static_cast<double>(deltaCpuUs) / deltaTimeUs) * 100;
					}
				}
				it->second.cpuUsageInUsec = process.cpuUsageInUsec;
				it->second.snapshotTime = process.snapshotTime;

			}
			else
			{
				processHistory[process.pid] = process;
			}
			std::cout << std::left
				<< std::setw(wName) << process.processName
				<< std::setw(wPid) << process.pid
				<< std::setw(wStatus) << RuntimeStateToString(process.runtimeHealth.state)
				<< std::setw(wMissed) << process.runtimeHealth.missedBeats
				<< std::setw(wStart) << process.runtimeHealth.restatCount
				<< std::setw(wCpu) << std::fixed << std::setprecision(1) << cpuPercentage
				<< std::setw(wMem) << std::fixed << std::setprecision(2) << memoryInMB << '\n';
		}
		std::cout << std::flush;
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
}

bool ProcessManagerApp::SendCommandMessage(ProcessManagerCommandEnum command, const std::string& serviceName)
{
	ProcessManagerCommandStruct message{};
	if (serviceName.size() >= sizeof(message.ServiceName))
	{
		cerr << "Service name '" << serviceName << "' is longer than " << sizeof(message.ServiceName) - 1 << " bytes" << endl;
		return false;
	}
	memcpy(message.ServiceName, serviceName.data(), serviceName.size());
	message.command = command;
	zmq::context_t context(1);
	zmq::socket_t s(context, zmq::socket_type::dealer);
	s.set(zmq::sockopt::routing_id, comm::COMMAND_SERVER_ID);
	// Give up when the manager is not running instead of waiting for it forever.
	s.set(zmq::sockopt::immediate, 1);
	s.set(zmq::sockopt::sndtimeo, 2000);
	s.set(zmq::sockopt::linger, 2000);
	s.connect(comm::COMMAND_SERVER_PORT);
	const bool sent = s.send(zmq::buffer(comm::PROCESS_MANAGER_ID), zmq::send_flags::sndmore)
		&& s.send(zmq::buffer((void*)&message, sizeof(ProcessManagerCommandStruct)), zmq::send_flags::none);
	if (!sent)
	{
		cerr << "Process manager is not reachable at " << comm::COMMAND_SERVER_PORT << endl;
	}
	return sent;
}

bool ProcessManagerApp::SendStartService(std::string serviceName)
{
	return ProcessManagerApp::SendCommandMessage(ProcessManagerCommandEnum::StartService, serviceName);
}
bool ProcessManagerApp::SendKillService(std::string serviceName)
{
	return ProcessManagerApp::SendCommandMessage(ProcessManagerCommandEnum::StopService, serviceName);
}
bool ProcessManagerApp::SendDisableService(std::string serviceName)
{
	return ProcessManagerApp::SendCommandMessage(ProcessManagerCommandEnum::DisableService, serviceName);
}
bool process_manager::ProcessManagerApp::SendRestartService(std::string serviceName)
{
	return ProcessManagerApp::SendCommandMessage(ProcessManagerCommandEnum::RestartService, serviceName);
}