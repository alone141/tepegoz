#include <cxxopts.hpp>
#include <ProcessManagerApp.h>
#include <thread>
#include <csignal>
#include <HelperFunctions.h>
#include <legacy/oslib/signal/SignalHelper.h>

#include <NetworkLogger.h>
#include <NetworkManager.h>

//meliksahs TODO 
process_manager::ProcessManagerApp* manager;
//process_manager::NetworkManager* networkMaster;
nwm::NetworkManager* gNetworkManager = nullptr;

static void SignalHandler(int signum)
{
	//networkMaster->TearDown();
	manager->ShutdownSignalReceived(signum);
}

int main(int argc, char** argv)
{
	legacy::oslib::SignalHelper::BlockSoftwareTimerSignals();

	if (ParseInputFlags(argc, argv))
	{
		return 0;
	}

	const char* portName = "tcp://*:5557";
	manager = new process_manager::ProcessManagerApp;
	//networkMaster = new process_manager::NetworkManager(portName);
	std::signal(SIGINT, SignalHandler);


	manager->StartUp();

	//std::thread networkManThread(&process_manager::NetworkManager::Run, networkMaster);
	std::thread processManThread(&process_manager::ProcessManagerApp::Run, manager);

	nwm::NetworkManager manager;
	gNetworkManager = &manager;
	const nwm::Status bindStatus = manager.Bind();
	if (!bindStatus.IsOk())
	{
		gNetworkManager = nullptr;
		nwm::LogError(std::string("Bind failed: ") +
			bindStatus.ToString());
		return 1;
	}
	const nwm::Status runStatus = manager.Run();

	processManThread.join();
	//networkManThread.join();


	//delete networkMaster;
	//delete manager; //timer segfault veriyor ????
	std::cout << "EXIT MAIN" << std::endl;

	return 0;
}

