#pragma once
#include <iostream>
#include <filesystem>
#include <sys/types.h>

#include <optional>
#include <vector>

namespace tepegoz
{

enum class KillCode
{
    Ok,
    NoGroup,
    StillForking,
};

class ProcessGroupBuilder
{
public:
    ProcessGroupBuilder(const ProcessGroupBuilder&) = delete;
    ProcessGroupBuilder& operator=(const ProcessGroupBuilder&) = delete;

    static ProcessGroupBuilder& GetInstance();
    static std::filesystem::path GetCgroupPathOf(pid_t pid); // its cgroup v2 directory; empty when unknown

    /**
     * @brief List the processes of one cgroup, as its cgroup.procs names them.
     * @param cgroup The cgroup's directory.
     * @return The pids, or std::nullopt when the file cannot be read. A line that is not a pid above 0 is left out:
     *         the kernel writes 0 for a process of another pid namespace, and kill() takes 0 or less as a whole
     *         group of processes.
     */
    static std::optional<std::vector<pid_t>> ReadGroupProcesses(const std::filesystem::path& cgroup);

    /**
     * @brief Send SIGKILL to every process in a cgroup and in the cgroups below it.
     * @param cgroup The cgroup's directory.
     * @return KillCode::Ok when the kernel took the request through cgroup.kill, or, where that file is missing
     *         (it exists from Linux 5.14), when every process the cgroup.procs files list was signalled.
     *         KillCode::NoGroup when the cgroup cannot be read, KillCode::StillForking when new processes kept
     *         appearing.
     */
    static KillCode KillGroup(const std::filesystem::path& cgroup);

    std::filesystem::path GetParentGroupPath();

    /**
     * @brief Create the services' cgroups under @p parent, in place of the cgroup the manager was started in.
     * @param parent The directory that holds task_pm and every task_<name>. A test gives a folder of its own.
     */
    void SetParentGroupPath(const std::filesystem::path& parent);

    void GenerateGroups() const;
private:
    std::filesystem::path GetProcessGroupPath();
    ProcessGroupBuilder();
    ~ProcessGroupBuilder();

    std::filesystem::path parentPath;
    std::filesystem::path managerPath;
    std::filesystem::path parentControlPath;
    std::filesystem::path managerProcPath;
};

} // namespace tepegoz
