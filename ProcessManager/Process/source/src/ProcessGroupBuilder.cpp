#include <ProcessGroupBuilder.h>
#include <fstream>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
namespace fs = std::filesystem;
using namespace process_manager;
ProcessGroupBuilder::ProcessGroupBuilder()
{
	this->parentPath = GetProcessGroupPath();
	this->parentControlPath = parentPath / "cgroup.subtree_control";
	this->managerPath = parentPath / "task_pm";
	this->managerProcPath = managerPath / "cgroup.procs";
}
ProcessGroupBuilder::~ProcessGroupBuilder()
{

}
ProcessGroupBuilder& ProcessGroupBuilder::GetInstance()
{
    static ProcessGroupBuilder instance;
    return instance;
}
std::filesystem::path ProcessGroupBuilder::GetParentGroupPath()
{
	return parentPath;
}
fs::path ProcessGroupBuilder::GetProcessGroupPath()
{
	std::ifstream cgroup_file("/proc/self/cgroup");
	std::string line;

	if (cgroup_file.is_open())
	{
		while (std::getline(cgroup_file, line))
		{
			// cgroup v2 format: "0::/path/to/cgroup"
			size_t pos = line.find("0::");
			if (pos != std::string::npos)
			{
				std::string path_suffix = line.substr(pos + 3);
				return fs::path("/sys/fs/cgroup") / fs::path(path_suffix).relative_path();
			}
		}
	}
	// Fallback if not running under systemd delegation or admin privs
	return fs::path("/sys/fs/cgroup");
}
void ProcessGroupBuilder::GenerateGroups() const
{
	std::error_code ec;
	if (!fs::create_directories(managerPath, ec) && ec)
	{
		printf("Failed to create cgroup directory:  %s ", strerror(errno));
	}

	std::ofstream managerProcFile(managerProcPath);
	if (managerProcFile.is_open())
	{
		std::string pid_str = std::to_string(getpid());
		managerProcFile << pid_str;
	}
	else
	{
		printf("Failed to write to proc file:  %s ", strerror(errno));

	}

	managerProcFile.close();

	std::ofstream parentControlFile(parentControlPath);
	if (parentControlFile.is_open())
	{
		parentControlFile << "+cpu +memory\n";
	}
	else
	{
		printf("Failed to write to parentControl file:  %s ", strerror(errno));

	}
	parentControlFile.close();
}