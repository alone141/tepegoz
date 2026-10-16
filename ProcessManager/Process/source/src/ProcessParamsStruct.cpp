#include <ProcessConfigParamsStruct.h>
#include <ConfigHelper.h>
using namespace process_manager;
int process_manager::ProcessConfigParams::UpdateArgs(const char* sectionOrder)
{
	const char* temp;
	int tempInt;

	auto retVal = ConfigHelper::ReadConfiguration(sectionOrder, "ProcessName", temp, "Process0");
	if (retVal != legacy::enums::RetVal::Success) return -1;

	this->ProcessName = std::string(temp);

	retVal = ConfigHelper::ReadConfiguration(sectionOrder, "ExecutablePath", temp, "invalid");
	if (retVal != legacy::enums::RetVal::Success) return -1;

	this->ExecutablePath = std::filesystem::path(temp);

	ConfigHelper::ReadConfiguration(sectionOrder, "Args", temp, "");
	ConfigHelper::SplitElementsToVector(temp, ' ', Args);

	ConfigHelper::ReadConfiguration(sectionOrder, "Cwd", temp, "");
	this->Cwd = std::filesystem::path(temp);

	ConfigHelper::ReadValue(sectionOrder, "InheritParentEnviroment", InheritParentEnviroment, true, 0, 1);
	ConfigHelper::ReadValue(sectionOrder, "UseDisplay", UseDisplay, false, 0, 1);

	ConfigHelper::ReadValue(sectionOrder, "CreateProcessGroup", CreateProcessGroup, true, 0, 1);

	//meliksahs TODO implement later
	//ConfigHelper::ReadValue(sectionOrder, "env_unset", env_unset, false, 0, 1);


	ConfigHelper::ReadValue(sectionOrder, "ForceKillTimeoutMs", tempInt, 10000, 1000, INT_MAX);
	ForceKillTimeoutMs = std::chrono::milliseconds(tempInt);

	ConfigHelper::ReadConfiguration(sectionOrder, "DependsOn", temp, "");
	ConfigHelper::SplitElementsToVector(temp, ' ', DependsOn);

	ConfigHelper::ReadConfiguration(sectionOrder, "EnviromentVariables", temp, "");
	ConfigHelper::SplitElementsToVector(temp, ';', EnviromentVariables);

	ConfigHelper::ReadConfiguration(sectionOrder, "WorkMode", temp, "Manual");
	if (!strcmp(temp, "Auto")) { this->WorkMode = WorkModeEnum::Auto; }
	else if (!strcmp(temp, "Manual")) { this->WorkMode = WorkModeEnum::Manual; }

	StdioPolicy.UpdateArgs(sectionOrder);
	RestartPolicy.UpdateArgs(sectionOrder);
	HealthPolicy.UpdateArgs(sectionOrder);

	return 1;
}
