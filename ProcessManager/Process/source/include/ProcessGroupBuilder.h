#pragma once
#include <iostream>
#include <filesystem>

//meliksahs bu buraya olmadi sanki
namespace process_manager
{
    class ProcessGroupBuilder
    {
    public:
        ProcessGroupBuilder(const ProcessGroupBuilder&) = delete;
        ProcessGroupBuilder& operator=(const ProcessGroupBuilder&) = delete;

        static ProcessGroupBuilder& GetInstance();
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
