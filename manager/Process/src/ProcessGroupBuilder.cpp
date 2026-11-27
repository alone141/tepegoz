#include "ProcessGroupBuilder.hpp"
#include <fstream>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include <algorithm>
#include <charconv>
#include <fcntl.h>
#include <optional>
#include <signal.h>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace tepegoz
{

namespace
{

// A process that forks while cgroup.procs is read can leave a child the read did not see. The files are read again
// until they name nothing new, but not more often than this, so a service that forks without end cannot hold the
// manager.
constexpr int max_kill_passes = 8;

// The cgroup v2 directory a /proc/<pid>/cgroup file names; empty when it names none.
fs::path ReadCgroupPath(const fs::path& cgroupFile)
{
    std::ifstream cgroup_file(cgroupFile);
    std::string line;

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
    return {};
}

// From Linux 5.14 the kernel kills a cgroup, with the cgroups below it, when "1" is written to its cgroup.kill.
// Opened without O_CREAT, so a kernel that has no such file is noticed.
bool WriteCgroupKill(const fs::path& cgroup)
{
    const int fd = ::open((cgroup / "cgroup.kill").c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return false;
    }

    const bool written = ::write(fd, "1", 1) == 1;
    ::close(fd);
    return written;
}

// The processes of a cgroup and of every cgroup below it: what cgroup.kill reaches.
std::optional<std::vector<pid_t>> ReadSubtreeProcesses(const fs::path& cgroup)
{
    std::optional<std::vector<pid_t>> processes = ProcessGroupBuilder::ReadGroupProcesses(cgroup);
    if (!processes.has_value())
    {
        return std::nullopt;
    }

    std::error_code error{};
    for (fs::recursive_directory_iterator entry{cgroup, error};
         !error && entry != fs::recursive_directory_iterator{}; entry.increment(error))
    {
        std::error_code ignored{};
        if (!entry->is_directory(ignored))
        {
            continue;
        }

        const std::optional<std::vector<pid_t>> below = ProcessGroupBuilder::ReadGroupProcesses(entry->path());
        if (below.has_value())
        {
            processes->insert(processes->end(), below->begin(), below->end());
        }
    }

    return processes;
}

} // namespace

ProcessGroupBuilder::ProcessGroupBuilder()
{
    SetParentGroupPath(GetProcessGroupPath());
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

void ProcessGroupBuilder::SetParentGroupPath(const fs::path& parent)
{
    parentPath = parent;
    parentControlPath = parentPath / "cgroup.subtree_control";
    managerPath = parentPath / "task_pm";
    managerProcPath = managerPath / "cgroup.procs";
}

std::optional<std::vector<pid_t>> ProcessGroupBuilder::ReadGroupProcesses(const fs::path& cgroup)
{
    std::ifstream procs{cgroup / "cgroup.procs"};
    if (!procs.is_open())
    {
        return std::nullopt;
    }

    std::vector<pid_t> processes{};
    std::string line{};
    while (std::getline(procs, line))
    {
        pid_t pid = 0;
        const char* const end = line.data() + line.size();
        const std::from_chars_result parsed = std::from_chars(line.data(), end, pid);
        if (parsed.ec == std::errc{} && parsed.ptr == end && pid > 0)
        {
            processes.push_back(pid);
        }
    }

    return processes;
}

KillCode ProcessGroupBuilder::KillGroup(const fs::path& cgroup)
{
    // An empty path would name the files of the working directory.
    if (cgroup.empty())
    {
        return KillCode::NoGroup;
    }

    if (WriteCgroupKill(cgroup))
    {
        return KillCode::Ok;
    }

    std::vector<pid_t> signalled{};
    for (int pass = 0; pass < max_kill_passes; ++pass)
    {
        const std::optional<std::vector<pid_t>> listed = ReadSubtreeProcesses(cgroup);
        if (!listed.has_value())
        {
            // On a later pass the cgroup was removed meanwhile, and nothing is left in it.
            return pass == 0 ? KillCode::NoGroup : KillCode::Ok;
        }

        bool foundNew = false;
        for (const pid_t pid : *listed)
        {
            if (std::find(signalled.begin(), signalled.end(), pid) != signalled.end())
            {
                continue; // it is listed until it has ended
            }

            ::kill(pid, SIGKILL);
            signalled.push_back(pid);
            foundNew = true;
        }

        if (!foundNew)
        {
            return KillCode::Ok;
        }
    }

    return KillCode::StillForking;
}

fs::path ProcessGroupBuilder::GetCgroupPathOf(pid_t pid)
{
    return ReadCgroupPath(fs::path("/proc") / std::to_string(pid) / "cgroup");
}
fs::path ProcessGroupBuilder::GetProcessGroupPath()
{
    const fs::path path = ReadCgroupPath("/proc/self/cgroup");
    if (!path.empty()) return path;
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

} // namespace tepegoz
