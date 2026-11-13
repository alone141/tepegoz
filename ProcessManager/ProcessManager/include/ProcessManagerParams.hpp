#pragma once
#include "Process.hpp"
#include <vector>
#include "WorkMode.hpp"
#include "ConfigSection.hpp"
#include "Logger.hpp"
namespace process_manager {
    struct ProcessManagerParams {
        WorkMode workMode = WorkMode::Manual;
        logger::Level logLevel = logger::Level::Debug; // [ProcessManager] LogLevel
        std::vector<Process> processList;
        config_section::ConfigValue document; // what the services were built from: "start all" builds them again
    };
}
