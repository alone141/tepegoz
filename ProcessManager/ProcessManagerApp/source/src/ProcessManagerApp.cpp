#include <ProcessManagerApp.h>
#include <ProcessManager.h>
#include "ConfigSection.hpp"
#include "Logger.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <unordered_map>
#include <thread>
#include <zmq.hpp>
using namespace process_manager;
using namespace std;


bool ProcessManagerApp::StartUp(const std::string& configPath)
{
	InitializeLogger();

	logger::Info("PROCESMANAGER VERSION: %s", PROCESSMANAGER_VERSION_FULL);


	// A file named with --config is used as given; only the installed one may fall back to the share directory.
	const bool givenPath = !configPath.empty();
	config_section::ConfigValue document;
	if (!LoadConfig(givenPath ? configPath : PROCESS_MANAGER_CONFIG_FILE, !givenPath, document)) return false;

	if (!InitializeProcessManager(document))
	{
		logger::Error("Process Manager failed to start");
		return false;
	}

	return true;
}

void ProcessManagerApp::Run()
{
	if (processManager) processManager->Run();
	logger::Debug("Exiting ProcessManager Thread");
}

// Called from main's signal thread: only asks the supervision thread to stop; Run() returns after.
void process_manager::ProcessManagerApp::ShutdownSignalReceived(int signum)
{
	logger::Warning(" ==== SHUTDOWN SIGNAL RECEIVED signum: %d  ==== ", signum);
	if (processManager) processManager->RequestStop();
}

bool ProcessManagerApp::LoadConfig(const std::string& configPath, bool allowFallback, config_section::ConfigValue& document)
{
	std::string path = configPath;
	std::string error;
	auto loaded = config_section::LoadConfigFile(path, document, error);
	if (loaded == config_section::LoadCode::FileNotOpened && allowFallback)
	{
		logger::Warning("Config file could not found at %s", path.c_str());
		path = PROCESS_MANAGER_SHARE_DIR"ProcessManagerConfig.yaml";
		logger::Warning("Searching config file at %s", path.c_str());
		loaded = config_section::LoadConfigFile(path, document, error);
	}

	if (loaded == config_section::LoadCode::FileNotOpened)
	{
		logger::Error("Config file %s could not be opened. Use --help flag for more information", path.c_str());
		return false;
	}
	if (loaded == config_section::LoadCode::SyntaxError)
	{
		logger::Error("Config file %s is not valid YAML: %s", path.c_str(), error.c_str());
		return false;
	}

	logger::Info("Configuration read from %s", path.c_str());
	return true;
}

bool ProcessManagerApp::InitializeProcessManager(const config_section::ConfigValue& document)
{
	auto config = ProcessManager::GenerateConfig(document);
	logger::Logger::Instance().SetMinimum(config->logLevel);
	try
	{
		processManager = make_unique<ProcessManager>(move(config));
	}
	catch (const zmq::error_t& e)
	{
		// For example another manager already holds the health ports.
		logger::Error("Process manager sockets could not be opened: %s", e.what());
		return false;
	}
	auto cycleDetected = !processManager->StartUp();
	return !cycleDetected;
}

void ProcessManagerApp::InitializeLogger()
{
	logger::Settings settings;
	settings.directory = PROCESS_MANAGER_LOG_DIR;
	const auto opened = logger::Logger::Instance().Open(settings);
	if (opened == logger::OpenCode::DirectoryFailed)
	{
		logger::Error("File log directory creation error: \"%s\"; logging to the console only", PROCESS_MANAGER_LOG_DIR);
	}
	else if (opened == logger::OpenCode::FileFailed)
	{
		logger::Error("No log file could be opened in \"%s\"; logging to the console only", PROCESS_MANAGER_LOG_DIR);
	}
}

void ProcessManagerApp::TearDown()
{
	if (processManager) processManager->TearDown();
	logger::Logger::Instance().Close();
}

bool ProcessManagerApp::ValidateConfig(string config)
{
	vector<string> err;
	config_section::ConfigValue document;
	if (!LoadConfig(config, false, document)) { return false; }
	auto params = ProcessManager::GenerateConfig(document, &err); // keys, values and service names
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
	config_section::ConfigValue document;
	if (!LoadConfig(config, false, document)) return;

	auto params = ProcessManager::GenerateConfig(document, &err);
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
				const auto elapsed = process.snapshotTime - it->second.snapshotTime;
				uint64_t deltaTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

				// Durations, so the units cannot mix; an interval that went back (clock change) is skipped too.
				if (elapsed > std::chrono::milliseconds(300))
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