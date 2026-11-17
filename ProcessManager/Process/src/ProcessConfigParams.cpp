#include "ProcessConfigParams.hpp"
#include "ConfigSection.hpp"
#include <climits>

namespace process_manager
{

// "pm" is the manager's own cgroup (task_pm), BPM/PMC/GUI are broker identities, "all" is the
// command target for every service, and a '/' would put task_<name> outside the cgroup directory.
bool ProcessConfigParams::IsReservedName(const std::string& name)
{
    for (const char* reserved : { "pm", "all", "BPM", "PMC", "GUI" })
    {
        if (name == reserved) return true;
    }
    return name.empty() || name.find('/') != std::string::npos;
}

int ProcessConfigParams::UpdateArgs(config_section::ConfigSection& section)
{
    this->processName = section.ReadString("ProcessName", "");
    if (processName.empty()) return -1;
    if (IsReservedName(processName)) return -2;

    this->executablePath = std::filesystem::path(section.ReadString("ExecutablePath", ""));
    if (executablePath.empty()) return -1;

    // A list keeps an argument with spaces in one piece; a text is split at spaces.
    args = section.ReadList("Args", ' ');
    this->cwd = std::filesystem::path(section.ReadString("Cwd", ""));

    // Older configurations spell these two keys as the code once read them.
    const bool bothSpellings = (section.Has("InheritParentEnvironment") && section.Has("InheritParentEnviroment")) ||
                               (section.Has("EnvironmentVariables") && section.Has("EnviromentVariables"));
    inheritParentEnvironment = section.ReadBool(section.RenamedKey("InheritParentEnvironment", "InheritParentEnviroment"), true);
    useDisplay = section.ReadBool("UseDisplay", false);
    this->display = section.ReadString("Display", ":1");
    this->xAuthority = section.ReadString("XAuthority", "/home/user/.Xauthority");

    createProcessGroup = section.ReadBool("CreateProcessGroup", true);


    forceKillTimeoutMs = std::chrono::milliseconds(section.ReadInt("ForceKillTimeoutMs", 10000, 1000, INT_MAX));

    dependsOn = section.ReadList("DependsOn", ' ');
    environmentVariables = section.ReadAssignments(section.RenamedKey("EnvironmentVariables", "EnviromentVariables"), ';');

    // The choices in WorkMode order; without one, [ProcessManager] Mode may apply (GenerateConfig).
    this->workMode = static_cast<WorkMode>(section.ReadChoice("WorkMode", { "Auto", "Manual" }, 1));

    stdioPolicy.UpdateArgs(section);
    restartPolicy.UpdateArgs(section);
    healthPolicy.UpdateArgs(section);

    return bothSpellings ? -3 : 1;
}

} // namespace process_manager
