#include <ProcessManagerApp.h>
#include <ProcessManager.h>
#include "ConfigSection.hpp"
#include "Logger.hpp"
#include <iostream>
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
