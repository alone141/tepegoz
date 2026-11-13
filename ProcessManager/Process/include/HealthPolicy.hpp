#pragma once
#include <chrono>
namespace config_section
{
class ConfigSection;
}
namespace process_manager
{
struct HealthPolicy
{
    void UpdateArgs(config_section::ConfigSection& section);

    bool checkHealth = false; // enable health checks
    std::chrono::milliseconds gracePeriodMs{ 5'000 }; //startup period

    std::chrono::milliseconds heartbeatPeriodMs{ 1'000 };
    int missedHeartbeatThreshold = 5; //take action after threshold
};
}