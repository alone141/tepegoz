#include "RestartPolicy.hpp"
#include "ConfigSection.hpp"
#include <climits>

namespace process_manager
{

void RestartPolicy::UpdateArgs(config_section::ConfigSection& section)
{
    // The choices in RestartMode order.
    restartMode = static_cast<RestartMode>(section.ReadChoice("RestartMode", { "Never", "OnFailure", "Always", "UnlessStopped" }, 1));

    maxRestarts = section.ReadInt("MaxRestarts", 5, 0, 1000);
    backoffMultiplier = section.ReadDouble("BackoffMultiplier", 2.0, 1.0, 10.0);
    restartDelayMs = std::chrono::milliseconds(section.ReadInt("RestartDelayMs", 1000, 0, INT_MAX));
    restartResetMs = std::chrono::milliseconds(section.ReadInt("RestartResetMs", 60000, 0, INT_MAX));
}

} // namespace process_manager
