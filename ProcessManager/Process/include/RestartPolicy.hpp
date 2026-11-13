#pragma once
#include "RestartMode.hpp"
#include <chrono>
namespace config_section
{
    class ConfigSection;
}
namespace process_manager
{
    struct RestartPolicyStruct
    {
        void UpdateArgs(config_section::ConfigSection& section);
        RestartModeEnum RestartMode = RestartModeEnum::OnFailure;
        int MaxRestarts = 10; // in a row; a run of RestartResetMs or longer starts the count again
        double BackoffMultiplier = 2.0;
        std::chrono::milliseconds RestartDelayMs{ 1'000 };
        std::chrono::milliseconds RestartResetMs{ 60'000 };
    };
}
