#include <RestartPolicyStruct.h>
#include "ConfigSection.hpp"
#include <climits>
using namespace process_manager;
void process_manager::RestartPolicyStruct::UpdateArgs(config_section::ConfigSection& section)
{
	// The choices in RestartModeEnum order.
	RestartMode = static_cast<RestartModeEnum>(section.ReadChoice("RestartMode", { "Never", "OnFailure", "Always", "UnlessStopped" }, 1));

	MaxRestarts = section.ReadInt("MaxRestarts", 5, 0, 1000);
	BackoffMultiplier = section.ReadDouble("BackoffMultiplier", 2.0, 1.0, 10.0);
	RestartDelayMs = std::chrono::milliseconds(section.ReadInt("RestartDelayMs", 1000, 0, INT_MAX));
	RestartResetMs = std::chrono::milliseconds(section.ReadInt("RestartResetMs", 60000, 0, INT_MAX));
}
