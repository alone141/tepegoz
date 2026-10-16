#include <HealthPolicyStruct.h>
#include <ConfigHelper.h>
using namespace process_manager;
void process_manager::HealthPolicyStruct::UpdateArgs(const char* sectionOrder)
{
	int temp = 0;
	ConfigHelper::ReadValue(sectionOrder, "CheckHealth", CheckHealth, false, 0, 1);
	ConfigHelper::ReadValue(sectionOrder, "GracePeriodMs", temp, 5000, 0, INT_MAX); GracePeriodMs = std::chrono::milliseconds(temp);
	ConfigHelper::ReadValue(sectionOrder, "HeartbeatPeriodMs", temp, 1000, 0, INT_MAX); HeartbeatPeriodMs = std::chrono::milliseconds(temp);
	ConfigHelper::ReadValue(sectionOrder, "MissedHeartbeatThreshold", MissedHeartbeatThreshold, 5, 0, 1000);
}
