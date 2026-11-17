#pragma once
#include "RestartMode.hpp"
#include <chrono>
namespace config_section
{

class ConfigSection;

} // namespace config_section
namespace process_manager
{

struct RestartPolicy
{
    void UpdateArgs(config_section::ConfigSection& section);
    RestartMode restartMode = RestartMode::OnFailure;
    int maxRestarts = 5; // in a row; a run of RestartResetMs or longer starts the count again
    double backoffMultiplier = 2.0;
    std::chrono::milliseconds restartDelayMs{ 1'000 };
    std::chrono::milliseconds restartResetMs{ 60'000 };
};

} // namespace process_manager
