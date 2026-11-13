#pragma once
#include <chrono>
namespace config_section
{
    class ConfigSection;
}
namespace process_manager
{
    struct HealthPolicyStruct
    {
        void UpdateArgs(config_section::ConfigSection& section);

        bool CheckHealth = false; // enable health checks
        std::chrono::milliseconds GracePeriodMs{ 5'000 }; //startup period

        std::chrono::milliseconds HeartbeatPeriodMs{ 1'000 };
        int MissedHeartbeatThreshold = 5; //take action after threshold
    };
}