#include <gtest/gtest.h>

#include "ProcessManager.hpp"
#include "ConfigSource.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

// A folder of its own for one test, removed with the test.
class ScratchFolder
{
public:
    ScratchFolder()
    {
        char pattern[] = "/tmp/process_manager_test_XXXXXX";
        const char* made = ::mkdtemp(pattern);
        path = made != nullptr ? std::filesystem::path{made} : std::filesystem::path{};
    }

    ~ScratchFolder()
    {
        Remove();
    }

    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;

    ScratchFolder(ScratchFolder&& other) noexcept
        : path{std::exchange(other.path, std::filesystem::path{})}
    {
    }

    ScratchFolder& operator=(ScratchFolder&& other) noexcept
    {
        if (this != &other)
        {
            Remove();
            path = std::exchange(other.path, std::filesystem::path{});
        }

        return *this;
    }

    void Write(const std::string& name, const std::string& content) const
    {
        const std::filesystem::path file = path / name;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream{file, std::ios::binary} << content;
    }

    std::filesystem::path MainFile() const
    {
        return path / "main.yaml";
    }

private:
    void Remove()
    {
        if (!path.empty())
        {
            std::error_code ignored{};
            std::filesystem::remove_all(path, ignored);
        }
    }

    std::filesystem::path path;
};

config_source::LoadedConfig Load(const ScratchFolder& folder)
{
    const config_source::ConfigSource source{folder.MainFile()};
    config_source::LoadedConfig loaded{};
    std::string error;
    EXPECT_EQ(source.Load(loaded, error), config_source::LoadCode::Ok) << error;
    EXPECT_TRUE(loaded.problems.empty());
    return loaded;
}

std::vector<std::string> Names(const std::vector<process_manager::Process>& processes)
{
    std::vector<std::string> names;
    for (const process_manager::Process& process : processes)
    {
        names.push_back(process.GetProcessName());
    }

    return names;
}

std::string Service(const std::string& name)
{
    return "ProcessName: " + name + "\nExecutablePath: /bin/true\n";
}

} // namespace

TEST(ProcessManagerTest, DropInServiceTakesTheManagerModeDefault)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "ProcessManager:\n  Mode: Auto\nServices: []\n");
    folder.Write("services.d/extra.yaml", Service("extra"));
    std::vector<std::string> errors;
    const std::unique_ptr<process_manager::ProcessManagerParams> params =
        process_manager::ProcessManager::GenerateConfig(Load(folder).document, &errors);
    EXPECT_TRUE(errors.empty());
    ASSERT_EQ(Names(params->processList), (std::vector<std::string>{"extra"}));
    EXPECT_FALSE(params->processList[0].IsWorkingManually());
}

TEST(ProcessManagerTest, DisabledDropInIsNotBuilt)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services:\n  - ProcessName: main\n    ExecutablePath: /bin/true\n");
    folder.Write("services.d/paused.yaml", Service("paused") + "Enabled: false\n");
    std::vector<std::string> errors;
    const std::unique_ptr<process_manager::ProcessManagerParams> params =
        process_manager::ProcessManager::GenerateConfig(Load(folder).document, &errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(Names(params->processList), (std::vector<std::string>{"main"}));
}

TEST(ProcessManagerTest, DependenciesCrossBetweenTheMainFileAndTheDropIns)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services:\n"
                              "  - ProcessName: back\n"
                              "    ExecutablePath: /bin/true\n"
                              "    DependsOn: [middle]\n"
                              "  - ProcessName: front\n"
                              "    ExecutablePath: /bin/true\n");
    folder.Write("services.d/middle.yaml", Service("middle") + "DependsOn: [front]\n");
    std::vector<std::string> errors;
    const std::unique_ptr<process_manager::ProcessManagerParams> params =
        process_manager::ProcessManager::GenerateConfig(Load(folder).document, &errors);
    EXPECT_TRUE(process_manager::ProcessManager::SortByDependencies(errors, params->processList));
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(Names(params->processList), (std::vector<std::string>{"front", "middle", "back"}));
}

TEST(ProcessManagerTest, DropInKeysAreCheckedLikeTheMainFileKeys)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services: []\n");
    folder.Write("services.d/extra.yaml", Service("extra") + "MaxRestart: 3\n");
    std::vector<std::string> errors;
    process_manager::ProcessManager::GenerateConfig(Load(folder).document, &errors);
    EXPECT_EQ(errors, (std::vector<std::string>{"service 'extra': unknown key 'MaxRestart'"}));
}

TEST(ProcessManagerTest, RebuildConfigKeepsTheDropInsAndWhereTheyCameFrom)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services:\n  - ProcessName: main\n    ExecutablePath: /bin/true\n");
    folder.Write("services.d/extra.yaml", Service("extra"));
    const config_source::LoadedConfig loaded = Load(folder);
    std::vector<std::string> errors;
    std::unique_ptr<process_manager::ProcessManagerParams> params =
        process_manager::ProcessManager::GenerateConfig(loaded.document, &errors);
    params->source = config_source::ConfigSource{loaded.mainFile};
    params->origins = loaded.origins;

    const std::unique_ptr<process_manager::ProcessManagerParams> rebuilt =
        process_manager::ProcessManager::RebuildConfig(*params);
    EXPECT_EQ(Names(rebuilt->processList), (std::vector<std::string>{"main", "extra"}));
    EXPECT_EQ(rebuilt->source.MainFile(), folder.MainFile());
    const config_source::ServiceOrigin* extra = config_source::FindOrigin(rebuilt->origins, "extra");
    ASSERT_NE(extra, nullptr);
    EXPECT_EQ(extra->kind, config_source::OriginKind::DropIn);
}
