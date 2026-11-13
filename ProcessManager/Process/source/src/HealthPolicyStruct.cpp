#include <HealthPolicyStruct.h>
#include "ConfigSection.hpp"
#include <climits>
using namespace process_manager;
void process_manager::HealthPolicyStruct::UpdateArgs(config_section::ConfigSection& section)
{
    CheckHealth = section.ReadBool("CheckHealth", false);
    GracePeriodMs = std::chrono::milliseconds(section.ReadInt("GracePeriodMs", 5000, 0, INT_MAX));
    HeartbeatPeriodMs = std::chrono::milliseconds(section.ReadInt("HeartbeatPeriodMs", 1000, 1, INT_MAX));
    MissedHeartbeatThreshold = section.ReadInt("MissedHeartbeatThreshold", 5, 1, 1000);
}
