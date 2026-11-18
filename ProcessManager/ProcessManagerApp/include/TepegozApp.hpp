#pragma once
#include "ITepegoz.hpp"
#include "TepegozCommands.hpp"

#include <memory>
#include <string>
#include <vector>

namespace config_source
{

struct LoadedConfig;

} // namespace config_source

namespace tepegoz
{

class TepegozApp
{
public:
    TepegozApp() = default;
    ~TepegozApp() = default;

    bool StartUp(const std::string& configPath = ""); // empty: the installed config; false: cannot run

    void TearDown();
    void Run();
    void ShutdownSignalReceived(int signum);

    // allowFallback: try the share directory's copy when configPath cannot be read. The services.d folder beside
    // the file that was read is read with it.
    static bool LoadConfig(const std::string& configPath, bool allowFallback, config_source::LoadedConfig& loaded);

    static bool ValidateConfig(std::string config); // true when no problem was found
    static void PrintDependencyGraph(std::string config);
private:
    static void InitializeLogger();
    bool InitializeManager(const config_source::LoadedConfig& loaded);
    static void PrintErrors(const std::vector<std::string>& errList);

    std::unique_ptr<ITepegoz> manager;
};

} // namespace tepegoz
