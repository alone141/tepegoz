#pragma once
#include <iostream>
#include <filesystem>
#include <sys/types.h>

//meliksahs bu buraya olmadi sanki
namespace process_manager
{
    class ProcessGroupBuilder
    {
    public:
        ProcessGroupBuilder(const ProcessGroupBuilder&) = delete;
        ProcessGroupBuilder& operator=(const ProcessGroupBuilder&) = delete;

        static ProcessGroupBuilder& GetInstance();
        static std::filesystem::path GetCgroupPathOf(pid_t pid); // its cgroup v2 directory; empty when unknown
        std::filesystem::path GetParentGroupPath();
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
}
