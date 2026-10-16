#include <cxxopts.hpp>
#include <ProcessManagerApp.h>
#include <thread>
#include <csignal>
#include <cstdlib>
#include <pthread.h>
#include <HelperFunctions.h>
#include <legacy/oslib/signal/SignalHelper.h>

#include <NetworkLogger.h>
#include <NetworkManager.h>

//meliksahs TODO
process_manager::ProcessManagerApp* manager;
//process_manager::NetworkManager* networkMaster;
nwm::NetworkManager* gNetworkManager = nullptr;

int main(int argc, char** argv)
{
	legacy::oslib::SignalHelper::BlockSoftwareTimerSignals();

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
	manager = new process_manager::ProcessManagerApp;
	//networkMaster = new process_manager::NetworkManager(portName);


	if (manager->StartUp(configPath) != legacy::enums::RetVal::Success)
	{
		manager->TearDown();
		return 1;
	}

	// Bind the broker before the process thread starts any service, so a failed bind has nothing to clean up.
	nwm::NetworkManager networkManager;
	gNetworkManager = &networkManager;
	const nwm::Status bindStatus = networkManager.Bind();
	if (!bindStatus.IsOk())
	{
		gNetworkManager = nullptr;
		nwm::LogError(std::string("Bind failed: ") +
			bindStatus.ToString());
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
			const nwm::Status runStatus = networkManager.Run();
			nwm::LogError(std::string("Broker stopped: ") + runStatus.ToString());
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
