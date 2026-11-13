#include "HealthPolicy.hpp"
#include "ConfigSection.hpp"
#include <climits>
using namespace process_manager;
void process_manager::HealthPolicy::UpdateArgs(config_section::ConfigSection& section)
{
    checkHealth = section.ReadBool("CheckHealth", false);
    gracePeriodMs = std::chrono::milliseconds(section.ReadInt("GracePeriodMs", 5000, 0, INT_MAX));
    heartbeatPeriodMs = std::chrono::milliseconds(section.ReadInt("HeartbeatPeriodMs", 1000, 1, INT_MAX));
    missedHeartbeatThreshold = section.ReadInt("MissedHeartbeatThreshold", 5, 1, 1000);
}
