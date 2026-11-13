#include <cxxopts.hpp>
#include <ProcessManagerApp.h>
#include <thread>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <pthread.h>
#include <string>
#include <HelperFunctions.h>

#include "Logger.hpp"
#include "NetworkManager.hpp"

process_manager::ProcessManagerApp* manager;
//process_manager::NetworkManager* networkMaster;
network_manager::NetworkManager* gNetworkManager = nullptr;

int main(int argc, char** argv)
{
	std::string configPath;
	const int exitCode = ParseInputFlags(argc, argv, configPath);
	if (exitCode >= 0)
	{
		return exitCode;
	}

	// SIGINT and SIGTERM are taken by one thread with sigwait instead of a handler, so stopping never
	// runs inside another thread's work. Blocked before any thread exists, so every thread inherits it.
	// Reset first, as installing the old handler did: one left ignored by the parent (a script's
	// background job ignores SIGINT) could be discarded before it reaches sigwait.
	std::signal(SIGINT, SIG_DFL);
	std::signal(SIGTERM, SIG_DFL);
	sigset_t shutdownSignals;
	sigemptyset(&shutdownSignals);
	sigaddset(&shutdownSignals, SIGINT);
	sigaddset(&shutdownSignals, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &shutdownSignals, nullptr);

	const char* portName = "tcp://*:5557";
	// The broker's status and events, for the monitor GUI's network page.
	const char* statusPortName = "tcp://*:6669";
	manager = new process_manager::ProcessManagerApp;
	//networkMaster = new process_manager::NetworkManager(portName);


	if (!manager->StartUp(configPath))
	{
		manager->TearDown();
		return 1;
	}

	// Bind the broker before the process thread starts any service, so a failed bind has nothing to clean up.
	network_manager::MonitorSettings monitorSettings{};
	monitorSettings.statusEndpoint = statusPortName;
	network_manager::NetworkManager networkManager{portName, monitorSettings};
	gNetworkManager = &networkManager;
	std::string bindError;
	if (networkManager.Bind(bindError) != network_manager::BindCode::Ok)
	{
		gNetworkManager = nullptr;
		logger::Error("Broker bind to %s failed: %s", portName, bindError.c_str());
		manager->TearDown();
		return 1;
	}

	std::thread signalThread([&shutdownSignals]
		{
			int signum = 0;
			sigwait(&shutdownSignals, &signum);
			manager->ShutdownSignalReceived(signum);
			sigwait(&shutdownSignals, &signum);
			std::_Exit(1); // a second signal: stop waiting for the services
		});
	std::thread networkManThread([&networkManager]
		{
			// Without the broker no heartbeat or command arrives, so stop rather than supervise blind.
			std::string runError;
			networkManager.Run(runError); // nothing asks it to stop, so it returns only on a failure
			logger::Error("Broker stopped: %s", runError.c_str());
			manager->ShutdownSignalReceived(0);
		});
	std::thread processManThread(&process_manager::ProcessManagerApp::Run, manager);

	processManThread.join(); // returns once every service is stopped
	manager->TearDown();


	//delete networkMaster;
	//delete manager; //timer segfault veriyor ????
	std::cout << "EXIT MAIN" << std::endl;

	// The broker and the signal thread have no way to be stopped; end the process without unwinding them.
	std::_Exit(0);
}
