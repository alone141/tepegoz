#pragma once
#include "Process.hpp"
#include <filesystem>
#include <vector>
#include "WorkMode.hpp"
#include "ConfigSection.hpp"
#include "ConfigSource.hpp"
#include "Logger.hpp"
namespace process_manager
{

struct ProcessManagerParams
{
    WorkMode workMode = WorkMode::Manual;
    logger::Level logLevel = logger::Level::Debug; // [ProcessManager] LogLevel
    std::vector<Process> processList;
    config_section::ConfigValue document; // what the services were built from: "start all" builds them again
    // Where document was read, so that it can be read again. Empty when it was not read from a file.
    config_source::ConfigSource source{std::filesystem::path{}};
    // Where each item of document's Services was defined: the main file or a file in its services.d.
    std::vector<config_source::ServiceOrigin> origins;
};

} // namespace process_manager
