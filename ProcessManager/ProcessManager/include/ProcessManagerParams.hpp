#pragma once
#include "Process.hpp"
#include <vector>
#include "WorkMode.hpp"
#include "ConfigSection.hpp"
#include "Logger.hpp"
namespace process_manager {
    struct ProcessManagerParams {
        WorkModeEnum workMode = WorkModeEnum::Manual;
        logger::Level logLevel = logger::Level::Debug; // [ProcessManager] LogLevel
        std::vector<Process> processList;
        config_section::ConfigValue document; // what the services were built from: "start all" builds them again
    };
}
