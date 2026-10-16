#include <RestartPolicyStruct.h>
#include <ConfigHelper.h>
using namespace process_manager;
void process_manager::RestartPolicyStruct::UpdateArgs(const char* sectionOrder)
{
	const char* temp;

	ConfigHelper::ReadConfiguration(sectionOrder, "RestartMode", temp, "OnFailure");
	if (!strcmp(temp, "Never")) { this->RestartMode = RestartModeEnum::Never; }
	else if (!strcmp(temp, "OnFailure")) { this->RestartMode = RestartModeEnum::OnFailure; }
	else if (!strcmp(temp, "Always")) { this->RestartMode = RestartModeEnum::Always; }
	else if (!strcmp(temp, "UnlessStopped")) { this->RestartMode = RestartModeEnum::UnlessStopped; }

	ConfigHelper::ReadValue(sectionOrder, "MaxRestarts", MaxRestarts, 5, 0, 1000);
	ConfigHelper::ReadValue(sectionOrder, "BackoffMultiplier", BackoffMultiplier, 2.0, 1.0, 10.0);

	int tempInt = 0;
	ConfigHelper::ReadValue(sectionOrder, "RestartDelayMs", tempInt, 1000, 0, INT_MAX); RestartDelayMs = std::chrono::milliseconds(tempInt);
	ConfigHelper::ReadValue(sectionOrder, "RestartResetMs", tempInt, 60000, 0, INT_MAX); RestartResetMs = std::chrono::milliseconds(tempInt);


}
