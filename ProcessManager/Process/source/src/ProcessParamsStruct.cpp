#include <ProcessConfigParamsStruct.h>
#include "ConfigSection.hpp"
#include <climits>
using namespace process_manager;

// "pm" is the manager's own cgroup (task_pm), BPM/PMC/GUI are broker identities, "all" is the
// command target for every service, and a '/' would put task_<name> outside the cgroup directory.
bool process_manager::ProcessConfigParams::IsReservedName(const std::string& name)
{
	for (const char* reserved : { "pm", "all", "BPM", "PMC", "GUI" })
	{
		if (name == reserved) return true;
	}
	return name.empty() || name.find('/') != std::string::npos;
}

int process_manager::ProcessConfigParams::UpdateArgs(config_section::ConfigSection& section)
{
	this->ProcessName = section.ReadString("ProcessName", "");
	if (ProcessName.empty()) return -1;
	if (IsReservedName(ProcessName)) return -2;

	this->ExecutablePath = std::filesystem::path(section.ReadString("ExecutablePath", ""));
	if (ExecutablePath.empty()) return -1;

	// A list keeps an argument with spaces in one piece; a text is split at spaces.
	Args = section.ReadList("Args", ' ');
	this->Cwd = std::filesystem::path(section.ReadString("Cwd", ""));

	InheritParentEnviroment = section.ReadBool("InheritParentEnviroment", true);
	UseDisplay = section.ReadBool("UseDisplay", false);
	this->Display = section.ReadString("Display", ":1");
	this->XAuthority = section.ReadString("XAuthority", "/home/user/.Xauthority");

	CreateProcessGroup = section.ReadBool("CreateProcessGroup", true);

	//meliksahs TODO implement later
	//env_unset


	ForceKillTimeoutMs = std::chrono::milliseconds(section.ReadInt("ForceKillTimeoutMs", 10000, 1000, INT_MAX));

	DependsOn = section.ReadList("DependsOn", ' ');
	EnviromentVariables = section.ReadAssignments("EnviromentVariables", ';');

	// The choices in WorkModeEnum order; without one, [ProcessManager] Mode may apply (GenerateConfig).
	this->WorkMode = static_cast<WorkModeEnum>(section.ReadChoice("WorkMode", { "Auto", "Manual" }, 1));

	StdioPolicy.UpdateArgs(section);
	RestartPolicy.UpdateArgs(section);
	HealthPolicy.UpdateArgs(section);

	return 1;
}
