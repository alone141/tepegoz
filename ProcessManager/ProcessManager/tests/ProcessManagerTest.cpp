#include <gtest/gtest.h>

#include "ProcessManager.hpp"
#include "ConfigSource.hpp"
#include "Logger.hpp"

#include <csignal>
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

// A service that runs until it is stopped, outside any cgroup, so that it runs anywhere.
std::string Sleeper(const std::string& name, const std::string& seconds = "60")
{
    return "ProcessName: " + name + "\nExecutablePath: /bin/sleep\nArgs: [" + seconds +
           "]\nCreateProcessGroup: false\nStdioMode: NullDevice\n";
}

// The parameters the manager starts with: built from the folder's configuration, in start order.
std::unique_ptr<process_manager::ProcessManagerParams> Started(const ScratchFolder& folder)
{
    const config_source::LoadedConfig loaded = Load(folder);
    std::vector<std::string> errors;
    std::unique_ptr<process_manager::ProcessManagerParams> params =
        process_manager::ProcessManager::GenerateConfig(loaded.document, &errors);
    EXPECT_TRUE(process_manager::ProcessManager::SortByDependencies(errors, params->processList));
    EXPECT_TRUE(errors.empty());
    params->source = config_source::ConfigSource{loaded.mainFile};
    params->origins = loaded.origins;
    return params;
}

process_manager::Process* Find(process_manager::ProcessManagerParams& params, const std::string& name)
{
    for (process_manager::Process& process : params.processList)
    {
        if (process.GetProcessName() == name)
        {
            return &process;
        }
    }

    return nullptr;
}

bool IsAlive(pid_t pid)
{
    return pid > 0 && ::kill(pid, 0) == 0;
}

// What a reload of the manager's parameters did, with the services it retired.
struct Reloaded
{
    process_manager::ReloadReport report{};
    std::vector<process_manager::Process> retiring;
};

Reloaded Reload(process_manager::ProcessManagerParams& params)
{
    Reloaded reloaded{};
    reloaded.report = process_manager::ProcessManager::Reload(params, reloaded.retiring);
    return reloaded;
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

TEST(ProcessManagerTest, ReloadAddsTheServiceOfANewFile)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "ProcessManager:\n  Mode: Auto\nServices:\n  - ProcessName: main\n    ExecutablePath: /bin/true\n");
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    folder.Write("services.d/extra.yaml", Service("extra"));
    const Reloaded reloaded = Reload(*params);
    EXPECT_EQ(reloaded.report.command, process_manager::CommandCode::ReloadConfig);
    EXPECT_EQ(reloaded.report.result, process_manager::ReloadResult::Applied);
    EXPECT_EQ(reloaded.report.added, 1);
    EXPECT_EQ(reloaded.report.unchanged, 1);
    EXPECT_EQ(Names(params->processList), (std::vector<std::string>{"main", "extra"}));
    const process_manager::Process* extra = Find(*params, "extra");
    ASSERT_NE(extra, nullptr);
    EXPECT_FALSE(extra->HasPid());
    EXPECT_FALSE(extra->IsWorkingManually());
}

TEST(ProcessManagerTest, ReloadPutsAddedServicesInStartOrder)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services:\n  - ProcessName: main\n    ExecutablePath: /bin/true\n    DependsOn: [late]\n");
    std::vector<std::string> ignored;
    const config_source::LoadedConfig first = Load(folder);
    std::unique_ptr<process_manager::ProcessManagerParams> params =
        process_manager::ProcessManager::GenerateConfig(first.document, &ignored);
    params->source = config_source::ConfigSource{first.mainFile};
    folder.Write("services.d/late.yaml", Service("late"));
    EXPECT_EQ(Reload(*params).report.result, process_manager::ReloadResult::Applied);
    EXPECT_EQ(Names(params->processList), (std::vector<std::string>{"late", "main"}));
}

TEST(ProcessManagerTest, ReloadRetiresTheServiceOfADeletedFile)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services:\n" "  - ProcessName: main\n    ExecutablePath: /bin/true\n");
    folder.Write("services.d/gone.yaml", Sleeper("gone"));
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    process_manager::Process* gone = Find(*params, "gone");
    ASSERT_NE(gone, nullptr);
    gone->StartProcess();
    const pid_t pid = gone->GetPid();
    ASSERT_TRUE(IsAlive(pid));

    std::filesystem::remove(folder.MainFile().parent_path() / "services.d" / "gone.yaml");
    Reloaded reloaded = Reload(*params);
    EXPECT_EQ(reloaded.report.removed, 1);
    EXPECT_EQ(Names(params->processList), (std::vector<std::string>{"main"}));
    ASSERT_EQ(reloaded.retiring.size(), 1U);
    EXPECT_EQ(reloaded.retiring[0].GetPid(), pid);
    EXPECT_TRUE(reloaded.retiring[0].IsStopping());
}

TEST(ProcessManagerTest, ReloadRemovesAServiceSwitchedOff)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services:\n" "  - ProcessName: main\n    ExecutablePath: /bin/true\n");
    folder.Write("services.d/paused.yaml", Service("paused"));
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    folder.Write("services.d/paused.yaml", Service("paused") + "Enabled: false\n");
    const Reloaded reloaded = Reload(*params);
    EXPECT_EQ(reloaded.report.removed, 1);
    EXPECT_TRUE(reloaded.retiring.empty());
    EXPECT_EQ(Names(params->processList), (std::vector<std::string>{"main"}));
}

TEST(ProcessManagerTest, ReloadLeavesARunningChangedServiceRunningWithTheMark)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services: []\n");
    folder.Write("services.d/edited.yaml", Sleeper("edited", "60"));
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    process_manager::Process* edited = Find(*params, "edited");
    ASSERT_NE(edited, nullptr);
    edited->StartProcess();
    const pid_t pid = edited->GetPid();

    folder.Write("services.d/edited.yaml", Sleeper("edited", "61"));
    const Reloaded reloaded = Reload(*params);
    EXPECT_EQ(reloaded.report.changed, 1);
    edited = Find(*params, "edited");
    ASSERT_NE(edited, nullptr);
    EXPECT_EQ(edited->GetPid(), pid);
    EXPECT_TRUE(IsAlive(pid));
    EXPECT_TRUE(edited->HasDeferredDefinition());
    EXPECT_EQ(edited->GetHealthReport().runtimeHealth.restartToApply, 1);
    EXPECT_TRUE(reloaded.retiring.empty());
}

TEST(ProcessManagerTest, ReloadDefersTheDefinitionOfAStoppedChangedService)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services:\n  - ProcessName: idle\n    ExecutablePath: /bin/true\n    WorkMode: Manual\n");
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    folder.Write("main.yaml", "Services:\n  - ProcessName: idle\n    ExecutablePath: /bin/true\n    WorkMode: Auto\n");
    EXPECT_EQ(Reload(*params).report.changed, 1);
    const process_manager::Process* idle = Find(*params, "idle");
    ASSERT_NE(idle, nullptr);
    EXPECT_TRUE(idle->HasDeferredDefinition());
    EXPECT_TRUE(idle->IsWorkingManually());
    EXPECT_FALSE(idle->HasPid());
}

TEST(ProcessManagerTest, ReloadLeavesAnUnchangedServiceUntouched)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services: []\n");
    folder.Write("services.d/same.yaml", Sleeper("same"));
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    process_manager::Process* same = Find(*params, "same");
    ASSERT_NE(same, nullptr);
    same->StartProcess();
    const pid_t pid = same->GetPid();
    const process_manager::RuntimeHealth before = same->GetRuntimeStats();

    folder.Write("services.d/same.yaml", "# reformatted\n" + Sleeper("same"));
    folder.Write("services.d/other.yaml", Service("other"));
    const Reloaded reloaded = Reload(*params);
    EXPECT_EQ(reloaded.report.unchanged, 1);
    same = Find(*params, "same");
    ASSERT_NE(same, nullptr);
    EXPECT_EQ(same->GetPid(), pid);
    EXPECT_EQ(same->GetRuntimeStats().startTime, before.startTime);
    EXPECT_EQ(same->GetRuntimeStats().restartCount, before.restartCount);
    EXPECT_FALSE(same->HasDeferredDefinition());
}

TEST(ProcessManagerTest, ReloadKeepsARunningServiceWhoseFileBroke)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services: []\n");
    folder.Write("services.d/fragile.yaml", Sleeper("fragile"));
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    process_manager::Process* fragile = Find(*params, "fragile");
    ASSERT_NE(fragile, nullptr);
    fragile->StartProcess();
    const pid_t pid = fragile->GetPid();

    folder.Write("services.d/fragile.yaml", "ProcessName: [fragile\n");
    const Reloaded reloaded = Reload(*params);
    EXPECT_EQ(reloaded.report.result, process_manager::ReloadResult::Applied);
    EXPECT_EQ(reloaded.report.kept, 1);
    EXPECT_TRUE(reloaded.retiring.empty());
    fragile = Find(*params, "fragile");
    ASSERT_NE(fragile, nullptr);
    EXPECT_EQ(fragile->GetPid(), pid);
    EXPECT_FALSE(fragile->HasDeferredDefinition());
}

TEST(ProcessManagerTest, ReorderingStopsNoRunningService)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services: []\n");
    folder.Write("services.d/a.yaml", Sleeper("a"));
    folder.Write("services.d/b.yaml", Sleeper("b"));
    folder.Write("services.d/c.yaml", Sleeper("c"));
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    std::vector<pid_t> pids;
    for (process_manager::Process& process : params->processList)
    {
        process.StartProcess();
        pids.push_back(process.GetPid());
    }

    // c first, then b, then a: every service changes place.
    folder.Write("services.d/a.yaml", Sleeper("a") + "DependsOn: [b]\n");
    folder.Write("services.d/b.yaml", Sleeper("b") + "DependsOn: [c]\n");
    folder.Write("services.d/new.yaml", Service("new"));
    const Reloaded reloaded = Reload(*params);
    EXPECT_EQ(reloaded.report.result, process_manager::ReloadResult::Applied);
    EXPECT_EQ(Names(params->processList), (std::vector<std::string>{"c", "new", "b", "a"}));
    for (const pid_t pid : pids)
    {
        EXPECT_TRUE(IsAlive(pid)) << pid;
    }
    EXPECT_EQ(Find(*params, "a")->GetPid(), pids[0]);
    EXPECT_EQ(Find(*params, "c")->GetPid(), pids[2]);
}

TEST(ProcessManagerTest, ReloadStoresTheNewDocumentForStartAll)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services:\n  - ProcessName: main\n    ExecutablePath: /bin/true\n");
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    folder.Write("services.d/extra.yaml", Service("extra") + "Args: [new]\n");
    ASSERT_EQ(Reload(*params).report.result, process_manager::ReloadResult::Applied);
    const config_source::ServiceOrigin* extra = config_source::FindOrigin(params->origins, "extra");
    ASSERT_NE(extra, nullptr);
    EXPECT_EQ(extra->kind, config_source::OriginKind::DropIn);
    const std::unique_ptr<process_manager::ProcessManagerParams> rebuilt =
        process_manager::ProcessManager::RebuildConfig(*params);
    EXPECT_EQ(Names(rebuilt->processList), (std::vector<std::string>{"main", "extra"}));
}

TEST(ProcessManagerTest, ReloadTakesTheLogLevelAndMode)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "ProcessManager:\n  Mode: Manual\n  LogLevel: Debug\nServices: []\n");
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    folder.Write("main.yaml", "ProcessManager:\n  Mode: Auto\n  LogLevel: Warning\nServices: []\n");
    ASSERT_EQ(Reload(*params).report.result, process_manager::ReloadResult::Applied);
    EXPECT_EQ(params->logLevel, logger::Level::Warning);
    EXPECT_EQ(params->workMode, process_manager::WorkMode::Auto);
    logger::Logger::Instance().SetMinimum(logger::Level::Debug);
}

TEST(ProcessManagerTest, ReloadOfABrokenMainFileChangesNothing)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services:\n  - ProcessName: main\n    ExecutablePath: /bin/true\n");
    folder.Write("services.d/extra.yaml", Service("extra"));
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    const config_section::ConfigValue document = params->document;
    folder.Write("main.yaml", "Services:\n  - ProcessName: main\n   ExecutablePath: /bin/true\n");
    folder.Write("services.d/added.yaml", Service("added"));
    const Reloaded reloaded = Reload(*params);
    EXPECT_EQ(reloaded.report.result, process_manager::ReloadResult::MainFileSyntaxError);
    EXPECT_EQ(reloaded.report.added, 0);
    EXPECT_EQ(Names(params->processList), (std::vector<std::string>{"main", "extra"}));
    EXPECT_EQ(params->document.keys, document.keys);
    EXPECT_TRUE(reloaded.retiring.empty());
}

TEST(ProcessManagerTest, ReloadKeepsARunningServiceWhoseEntryLostItsName)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services:\n  - ProcessName: main\n    ExecutablePath: /bin/sleep\n    Args: [60]\n"
                              "    CreateProcessGroup: false\n    StdioMode: NullDevice\n");
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    process_manager::Process* main = Find(*params, "main");
    ASSERT_NE(main, nullptr);
    main->StartProcess();
    const pid_t pid = main->GetPid();

    folder.Write("main.yaml", "Services:\n  - ProcessName: [main]\n    ExecutablePath: /bin/sleep\n");
    const Reloaded reloaded = Reload(*params);
    EXPECT_EQ(reloaded.report.result, process_manager::ReloadResult::Applied);
    EXPECT_EQ(reloaded.report.kept, 1);
    EXPECT_EQ(reloaded.report.removed, 0);
    EXPECT_TRUE(reloaded.retiring.empty());
    main = Find(*params, "main");
    ASSERT_NE(main, nullptr);
    EXPECT_EQ(main->GetPid(), pid);
}

TEST(ProcessManagerTest, ReloadOfAMissingMainFileChangesNothing)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services:\n  - ProcessName: main\n    ExecutablePath: /bin/true\n");
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    std::filesystem::remove(folder.MainFile());
    EXPECT_EQ(Reload(*params).report.result, process_manager::ReloadResult::MainFileNotOpened);
    EXPECT_EQ(Names(params->processList), (std::vector<std::string>{"main"}));
}

TEST(ProcessManagerTest, ReloadWithAnUnreadableFolderChangesNothing)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services: []\n");
    folder.Write("services.d/extra.yaml", Service("extra"));
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    const std::filesystem::path dropIns = folder.MainFile().parent_path() / "services.d";
    std::filesystem::remove_all(dropIns);
    std::filesystem::create_symlink("services.d", dropIns); // a link to itself cannot be read
    const Reloaded reloaded = Reload(*params);
    EXPECT_EQ(reloaded.report.result, process_manager::ReloadResult::FolderNotReadable);
    EXPECT_EQ(reloaded.report.removed, 0);
    EXPECT_EQ(Names(params->processList), (std::vector<std::string>{"extra"}));
}

TEST(ProcessManagerTest, ReloadWithoutAServicesListChangesNothing)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services:\n  - ProcessName: main\n    ExecutablePath: /bin/true\n");
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    folder.Write("main.yaml", "Services:\n  ProcessName: main\n");
    EXPECT_EQ(Reload(*params).report.result, process_manager::ReloadResult::NoServicesList);
    EXPECT_EQ(Names(params->processList), (std::vector<std::string>{"main"}));
}

TEST(ProcessManagerTest, ReloadWithACycleChangesNothing)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services: []\n");
    folder.Write("services.d/a.yaml", Service("a"));
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    folder.Write("services.d/a.yaml", Service("a") + "DependsOn: [b]\n");
    folder.Write("services.d/b.yaml", Service("b") + "DependsOn: [a]\n");
    EXPECT_EQ(Reload(*params).report.result, process_manager::ReloadResult::DependencyCycle);
    EXPECT_EQ(Names(params->processList), (std::vector<std::string>{"a"}));
    EXPECT_FALSE(Find(*params, "a")->HasDeferredDefinition());
}

TEST(ProcessManagerTest, ReloadWithADuplicateNameChangesNothing)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services:\n  - ProcessName: main\n    ExecutablePath: /bin/true\n");
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    folder.Write("main.yaml", "Services:\n  - ProcessName: main\n    ExecutablePath: /bin/true\n"
                              "  - ProcessName: main\n    ExecutablePath: /bin/false\n");
    EXPECT_EQ(Reload(*params).report.result, process_manager::ReloadResult::DuplicateName);
    EXPECT_EQ(Names(params->processList), (std::vector<std::string>{"main"}));
}

TEST(ProcessManagerTest, ReloadWithoutASourceIsRefused)
{
    process_manager::ProcessManagerParams params{};
    std::vector<process_manager::Process> retiring;
    EXPECT_EQ(process_manager::ProcessManager::Reload(params, retiring).result, process_manager::ReloadResult::NoSource);
}

TEST(ProcessManagerTest, ServiceDisabledAtRunTimeComesBackOnReload)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services:\n  - ProcessName: main\n    ExecutablePath: /bin/true\n");
    folder.Write("services.d/extra.yaml", Service("extra"));
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    params->processList.pop_back(); // what --disable extra does to the list
    EXPECT_EQ(Reload(*params).report.added, 1);
    EXPECT_EQ(Names(params->processList), (std::vector<std::string>{"main", "extra"}));
}

namespace
{

const std::string auto_main = "ProcessManager:\n  Mode: Auto\nServices:\n  - ProcessName: main\n    ExecutablePath: /bin/true\n";

// A registry request's reply, read back with the configuration reader: JSON is YAML.
struct Answered
{
    service_registry::ReplyStatus status = service_registry::ReplyStatus::Error;
    config_section::ConfigValue body;
    std::string json;
    std::vector<process_manager::Process> retiring;
};

Answered Ask(process_manager::ProcessManagerParams& params, const std::vector<std::string>& frames)
{
    Answered answered{};
    const service_registry::Reply reply = process_manager::ProcessManager::HandleRegistry(params, answered.retiring, frames);
    answered.status = reply.status;
    answered.json = reply.json;
    std::string error;
    EXPECT_EQ(config_section::ParseConfigText(reply.json, answered.body, error), config_section::LoadCode::Ok)
        << error << "\n" << reply.json;
    return answered;
}

std::string Field(const Answered& answered, const std::string& key)
{
    const config_section::ConfigValue* value = config_section::FindValue(answered.body, key);
    return value != nullptr ? value->text : std::string{};
}

bool HasProblemWith(const Answered& answered, const std::string& part)
{
    const config_section::ConfigValue* problems = config_section::FindValue(answered.body, "problems");
    if (problems == nullptr)
    {
        return false;
    }

    for (const config_section::ConfigValue& problem : problems->children)
    {
        if (problem.text.find(part) != std::string::npos)
        {
            return true;
        }
    }

    return false;
}

std::string Indented(const std::string& text)
{
    std::string out;
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = text.find('\n', start);
        end = end == std::string::npos ? text.size() : end;
        out += "  " + text.substr(start, end - start) + "\n";
        start = end + 1;
    }

    return out;
}

// The parameters of an add or a replace: the name, the definition, and any further lines.
std::string WithDefinition(const std::string& name, const std::string& definition, const std::string& more = "")
{
    return "name: " + name + "\ndefinition:\n" + Indented(definition) + more;
}

std::filesystem::path RegisteredFile(const ScratchFolder& folder, const std::string& name)
{
    return folder.MainFile().parent_path() / "services.d" / (name + ".yaml");
}

// As Started, for a folder whose files have problems on purpose.
std::unique_ptr<process_manager::ProcessManagerParams> StartedWithProblems(const ScratchFolder& folder)
{
    const config_source::ConfigSource source{folder.MainFile()};
    config_source::LoadedConfig loaded{};
    std::string error;
    EXPECT_EQ(source.Load(loaded, error), config_source::LoadCode::Ok) << error;
    EXPECT_FALSE(loaded.problems.empty());
    std::vector<std::string> ignored;
    std::unique_ptr<process_manager::ProcessManagerParams> params =
        process_manager::ProcessManager::GenerateConfig(loaded.document, &ignored);
    process_manager::ProcessManager::SortByDependencies(ignored, params->processList);
    params->source = config_source::ConfigSource{loaded.mainFile};
    params->origins = loaded.origins;
    return params;
}

config_section::ConfigValue ReadBack(const std::filesystem::path& file)
{
    config_section::ConfigValue value{};
    std::string error;
    EXPECT_EQ(config_section::LoadConfigFile(file, value, error), config_section::LoadCode::Ok) << error;
    return value;
}

} // namespace

TEST(ProcessManagerTest, RegistryAddWritesTheFileAndStartsAnAutomaticService)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", auto_main);
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    const Answered answered = Ask(*params, {"add", WithDefinition("extra", "ExecutablePath: /bin/true\nArgs: [\"--port\", \"8080\"]\n")});
    ASSERT_EQ(answered.status, service_registry::ReplyStatus::Ok) << answered.json;
    EXPECT_EQ(Field(answered, "outcome"), "starting");
    const config_section::ConfigValue written = ReadBack(RegisteredFile(folder, "extra"));
    ASSERT_NE(config_section::FindValue(written, "ProcessName"), nullptr);
    EXPECT_EQ(config_section::FindValue(written, "ProcessName")->text, "extra");
    EXPECT_EQ(config_section::FindValue(written, "Args")->children[1].text, "8080");
    EXPECT_EQ(Names(params->processList), (std::vector<std::string>{"main", "extra"}));
    const config_source::ServiceOrigin* origin = config_source::FindOrigin(params->origins, "extra");
    ASSERT_NE(origin, nullptr);
    EXPECT_EQ(origin->kind, config_source::OriginKind::DropIn);
}

TEST(ProcessManagerTest, RegistryAddMakesTheFolder)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", auto_main);
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    ASSERT_FALSE(std::filesystem::exists(folder.MainFile().parent_path() / "services.d"));
    EXPECT_EQ(Ask(*params, {"add", WithDefinition("extra", Service("extra"))}).status, service_registry::ReplyStatus::Ok);
    EXPECT_TRUE(std::filesystem::is_regular_file(RegisteredFile(folder, "extra")));
}

TEST(ProcessManagerTest, RegistryAddHeldKeepsTheServiceStoppedAndTheFileAsSent)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", auto_main);
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    const Answered answered = Ask(*params, {"add", WithDefinition("extra", Service("extra"), "start: false\n")});
    ASSERT_EQ(answered.status, service_registry::ReplyStatus::Ok) << answered.json;
    EXPECT_EQ(Field(answered, "outcome"), "held");
    const process_manager::Process* extra = Find(*params, "extra");
    ASSERT_NE(extra, nullptr);
    EXPECT_TRUE(extra->IsWorkingManually());
    EXPECT_EQ(config_section::FindValue(ReadBack(RegisteredFile(folder, "extra")), "WorkMode"), nullptr);
}

TEST(ProcessManagerTest, RegistryAddOfAManualServiceSaysHowToStartIt)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services:\n  - ProcessName: main\n    ExecutablePath: /bin/true\n");
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    const Answered answered = Ask(*params, {"add", WithDefinition("extra", Service("extra"))});
    ASSERT_EQ(answered.status, service_registry::ReplyStatus::Ok) << answered.json;
    EXPECT_EQ(Field(answered, "outcome"), "manual");
    EXPECT_NE(Field(answered, "message").find("--start extra"), std::string::npos);
}

TEST(ProcessManagerTest, RegistryAddOfAMissingExecutableWritesNothing)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", auto_main);
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    const Answered answered = Ask(*params, {"add", WithDefinition("extra", "ExecutablePath: /nonexistent/program\n")});
    EXPECT_EQ(answered.status, service_registry::ReplyStatus::Refused);
    EXPECT_TRUE(HasProblemWith(answered, "extra ExecutablePath does not exists")) << answered.json;
    EXPECT_FALSE(std::filesystem::exists(folder.MainFile().parent_path() / "services.d"));
    EXPECT_EQ(Names(params->processList), (std::vector<std::string>{"main"}));
}

TEST(ProcessManagerTest, RegistryAddOfAnUnknownKeyIsRefused)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", auto_main);
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    const Answered answered = Ask(*params, {"add", WithDefinition("extra", "ExecutablePath: /bin/true\nMaxRestart: 3\n")});
    EXPECT_EQ(answered.status, service_registry::ReplyStatus::Refused);
    EXPECT_TRUE(HasProblemWith(answered, "MaxRestart")) << answered.json;
}

TEST(ProcessManagerTest, RegistryAddIgnoresProblemsThatWereThereBefore)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "ProcessManager:\n  Mode: Auto\nServices:\n  - ProcessName: old\n"
                              "    ExecutablePath: /nonexistent/old\n    WorkMode: Manual\n");
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    const Answered answered = Ask(*params, {"add", WithDefinition("extra", Service("extra"))});
    EXPECT_EQ(answered.status, service_registry::ReplyStatus::Ok) << answered.json;
}

TEST(ProcessManagerTest, RegistryAddOfANameTheMainFileHasIsRefused)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", auto_main);
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    const Answered answered = Ask(*params, {"add", WithDefinition("main", Service("main"))});
    EXPECT_EQ(answered.status, service_registry::ReplyStatus::Refused);
    EXPECT_NE(Field(answered, "message").find("main file"), std::string::npos);
}

TEST(ProcessManagerTest, RegistryAddOfARegisteredNameIsRefusedEvenWhenItsFileIsBroken)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", auto_main);
    folder.Write("services.d/extra.yaml", "ProcessName: other\n");
    std::unique_ptr<process_manager::ProcessManagerParams> params = StartedWithProblems(folder);
    EXPECT_EQ(Ask(*params, {"add", WithDefinition("extra", Service("extra"))}).status,
              service_registry::ReplyStatus::Refused);
}

TEST(ProcessManagerTest, RegistryAddRefusesNamesThatCannotBeFilesOrAreReserved)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", auto_main);
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    EXPECT_EQ(Ask(*params, {"add", "name: a/b\ndefinition: {ExecutablePath: /bin/true}\n"}).status,
              service_registry::ReplyStatus::Refused);
    EXPECT_EQ(Ask(*params, {"add", "name: .hidden\ndefinition: {ExecutablePath: /bin/true}\n"}).status,
              service_registry::ReplyStatus::Refused);
    EXPECT_EQ(Ask(*params, {"add", "name: all\ndefinition: {ExecutablePath: /bin/true}\n"}).status,
              service_registry::ReplyStatus::Refused);
    EXPECT_FALSE(std::filesystem::exists(folder.MainFile().parent_path() / "services.d"));
}

TEST(ProcessManagerTest, RegistryAddOfADefinitionNamingAnotherServiceIsRefused)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", auto_main);
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    EXPECT_EQ(Ask(*params, {"add", WithDefinition("extra", Service("other"))}).status,
              service_registry::ReplyStatus::Refused);
}

TEST(ProcessManagerTest, RegistryDryRunWritesNothing)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", auto_main);
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    const Answered answered = Ask(*params, {"add", WithDefinition("extra", "ExecutablePath: /bin/true\n", "dryRun: true\n")});
    ASSERT_EQ(answered.status, service_registry::ReplyStatus::Ok) << answered.json;
    EXPECT_EQ(Field(answered, "outcome"), "valid");
    EXPECT_EQ(Field(answered, "yaml"), "ProcessName: extra\nExecutablePath: /bin/true\n");
    EXPECT_FALSE(std::filesystem::exists(RegisteredFile(folder, "extra")));
    EXPECT_EQ(Names(params->processList), (std::vector<std::string>{"main"}));
}

TEST(ProcessManagerTest, RegistryReplaceOfARunningServiceWaitsForItsNextStart)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services: []\n");
    folder.Write("services.d/edited.yaml", Sleeper("edited", "60"));
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    process_manager::Process* edited = Find(*params, "edited");
    ASSERT_NE(edited, nullptr);
    edited->StartProcess();
    const pid_t pid = edited->GetPid();

    const Answered answered = Ask(*params, {"replace", WithDefinition("edited", Sleeper("edited", "61"))});
    ASSERT_EQ(answered.status, service_registry::ReplyStatus::Ok) << answered.json;
    EXPECT_EQ(Field(answered, "outcome"), "waiting");
    edited = Find(*params, "edited");
    ASSERT_NE(edited, nullptr);
    EXPECT_EQ(edited->GetPid(), pid);
    EXPECT_TRUE(edited->HasDeferredDefinition());
    EXPECT_EQ(config_section::FindValue(ReadBack(RegisteredFile(folder, "edited")), "Args")->children[0].text, "61");
}

TEST(ProcessManagerTest, RegistryReplaceWithRestartRestartsARunningService)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services: []\n");
    folder.Write("services.d/edited.yaml", Sleeper("edited", "60"));
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    process_manager::Process* edited = Find(*params, "edited");
    ASSERT_NE(edited, nullptr);
    edited->StartProcess();

    const Answered answered =
        Ask(*params, {"replace", WithDefinition("edited", Sleeper("edited", "61"), "restart: true\n")});
    ASSERT_EQ(answered.status, service_registry::ReplyStatus::Ok) << answered.json;
    EXPECT_EQ(Field(answered, "outcome"), "restarting");
    edited = Find(*params, "edited");
    ASSERT_NE(edited, nullptr);
    EXPECT_TRUE(edited->IsRestartRequested());
    EXPECT_TRUE(edited->IsStopping());
}

TEST(ProcessManagerTest, RegistryReplaceWithRestartOfAStoppedServiceAppliesTheDefinition)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services: []\n");
    folder.Write("services.d/edited.yaml", Service("edited"));
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    const Answered answered = Ask(
        *params, {"replace", WithDefinition("edited", Service("edited") + "Args: [x]\n", "restart: true\n")});
    ASSERT_EQ(answered.status, service_registry::ReplyStatus::Ok) << answered.json;
    EXPECT_EQ(Field(answered, "outcome"), "applied");
    const process_manager::Process* edited = Find(*params, "edited");
    ASSERT_NE(edited, nullptr);
    EXPECT_FALSE(edited->HasDeferredDefinition());
    EXPECT_FALSE(edited->HasPid());
}

TEST(ProcessManagerTest, RegistryUpdateSetsAndUnsetsKeysAndKeepsTheRest)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services: []\n");
    folder.Write("services.d/edited.yaml", Service("edited") + "Args: [a, b]\nCwd: /tmp\n");
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    const Answered answered = Ask(*params, {"update", "name: edited\nset: {MaxRestarts: 3}\nunset: [Cwd]\n"});
    ASSERT_EQ(answered.status, service_registry::ReplyStatus::Ok) << answered.json;
    EXPECT_EQ(Field(answered, "outcome"), "waiting");
    const config_section::ConfigValue written = ReadBack(RegisteredFile(folder, "edited"));
    EXPECT_EQ(config_section::FindValue(written, "MaxRestarts")->text, "3");
    EXPECT_EQ(config_section::FindValue(written, "Cwd"), nullptr);
    EXPECT_EQ(config_section::FindValue(written, "Args")->children.size(), 2U);
    EXPECT_EQ(config_section::FindValue(written, "ExecutablePath")->text, "/bin/true");
}

TEST(ProcessManagerTest, RegistryUpdateThatChangesNothingSaysSo)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services: []\n");
    folder.Write("services.d/edited.yaml", Service("edited"));
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    const Answered answered = Ask(*params, {"update", "name: edited\nset: {ExecutablePath: /bin/true}\n"});
    ASSERT_EQ(answered.status, service_registry::ReplyStatus::Ok) << answered.json;
    EXPECT_EQ(Field(answered, "outcome"), "unchanged");
}

TEST(ProcessManagerTest, RegistryUpdateOfABrokenFileIsRefusedAndReplaceRepairsIt)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services: []\n");
    folder.Write("services.d/broken.yaml", "- not a mapping\n");
    std::unique_ptr<process_manager::ProcessManagerParams> params = StartedWithProblems(folder);
    EXPECT_EQ(Ask(*params, {"update", "name: broken\nset: {Cwd: /tmp}\n"}).status, service_registry::ReplyStatus::Refused);
    const Answered answered = Ask(*params, {"replace", WithDefinition("broken", Service("broken"))});
    ASSERT_EQ(answered.status, service_registry::ReplyStatus::Ok) << answered.json;
    EXPECT_EQ(Field(answered, "outcome"), "manual");
    EXPECT_NE(Find(*params, "broken"), nullptr);
}

TEST(ProcessManagerTest, RegistryChangeOfAServiceOfTheMainFileIsRefused)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", auto_main);
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    EXPECT_EQ(Ask(*params, {"replace", WithDefinition("main", Service("main"))}).status,
              service_registry::ReplyStatus::Refused);
    EXPECT_EQ(Ask(*params, {"update", "name: main\nset: {Cwd: /tmp}\n"}).status, service_registry::ReplyStatus::Refused);
    const Answered removed = Ask(*params, {"remove", "name: main\n"});
    EXPECT_EQ(removed.status, service_registry::ReplyStatus::Refused);
    EXPECT_NE(Field(removed, "message").find("main file"), std::string::npos);
}

TEST(ProcessManagerTest, RegistryRemoveDeletesTheFileAndRetiresTheService)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services: []\n");
    folder.Write("services.d/gone.yaml", Sleeper("gone"));
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    process_manager::Process* gone = Find(*params, "gone");
    ASSERT_NE(gone, nullptr);
    gone->StartProcess();
    const pid_t pid = gone->GetPid();

    const Answered answered = Ask(*params, {"remove", "name: gone\n"});
    ASSERT_EQ(answered.status, service_registry::ReplyStatus::Ok) << answered.json;
    EXPECT_EQ(Field(answered, "outcome"), "removed");
    EXPECT_FALSE(std::filesystem::exists(RegisteredFile(folder, "gone")));
    EXPECT_TRUE(params->processList.empty());
    ASSERT_EQ(answered.retiring.size(), 1U);
    EXPECT_EQ(answered.retiring[0].GetPid(), pid);
}

TEST(ProcessManagerTest, RegistryRemoveOfANeededServiceIsRefusedUnlessForced)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services:\n  - ProcessName: user\n    ExecutablePath: /bin/true\n    DependsOn: [needed]\n");
    folder.Write("services.d/needed.yaml", Service("needed"));
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    const Answered refused = Ask(*params, {"remove", "name: needed\n"});
    EXPECT_EQ(refused.status, service_registry::ReplyStatus::Refused);
    EXPECT_TRUE(HasProblemWith(refused, "user depends on 'needed'")) << refused.json;
    EXPECT_TRUE(std::filesystem::exists(RegisteredFile(folder, "needed")));

    const Answered forced = Ask(*params, {"remove", "name: needed\nforce: true\n"});
    ASSERT_EQ(forced.status, service_registry::ReplyStatus::Ok) << forced.json;
    EXPECT_FALSE(std::filesystem::exists(RegisteredFile(folder, "needed")));
    EXPECT_EQ(Names(params->processList), (std::vector<std::string>{"user"}));
}

TEST(ProcessManagerTest, RegistryRemoveOfAnUnknownServiceIsRefused)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", auto_main);
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    EXPECT_EQ(Ask(*params, {"remove", "name: nobody\n"}).status, service_registry::ReplyStatus::Refused);
}

TEST(ProcessManagerTest, RegistryListGivesEveryServiceWithItsOrigin)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", auto_main);
    folder.Write("services.d/extra.yaml", Service("extra"));
    folder.Write("services.d/broken.yaml", "ProcessName: other\n");
    std::unique_ptr<process_manager::ProcessManagerParams> params = StartedWithProblems(folder);
    const Answered answered = Ask(*params, {"list"});
    ASSERT_EQ(answered.status, service_registry::ReplyStatus::Ok) << answered.json;
    const config_section::ConfigValue* services = config_section::FindValue(answered.body, "services");
    ASSERT_NE(services, nullptr);
    ASSERT_EQ(services->children.size(), 3U) << answered.json;
    EXPECT_EQ(config_section::FindValue(services->children[0], "name")->text, "main");
    EXPECT_EQ(config_section::FindValue(services->children[0], "origin")->text, "main");
    EXPECT_EQ(config_section::FindValue(services->children[1], "name")->text, "extra");
    EXPECT_EQ(config_section::FindValue(services->children[1], "origin")->text, "registered");
    EXPECT_EQ(config_section::FindValue(services->children[1], "loaded")->text, "true");
    EXPECT_EQ(config_section::FindValue(services->children[2], "name")->text, "broken");
    EXPECT_NE(config_section::FindValue(services->children[2], "problem"), nullptr);
}

TEST(ProcessManagerTest, RegistryShowGivesTheDefinitionAsJsonAndYaml)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", auto_main);
    folder.Write("services.d/extra.yaml", Service("extra") + "Args: [\"8080\"]\n");
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    const Answered answered = Ask(*params, {"show", "name: extra\n"});
    ASSERT_EQ(answered.status, service_registry::ReplyStatus::Ok) << answered.json;
    EXPECT_EQ(Field(answered, "origin"), "registered");
    const config_section::ConfigValue* definition = config_section::FindValue(answered.body, "definition");
    ASSERT_NE(definition, nullptr);
    EXPECT_EQ(config_section::FindValue(*definition, "ExecutablePath")->text, "/bin/true");
    EXPECT_EQ(config_section::FindValue(*definition, "Args")->children[0].kind, config_section::ValueKind::String);
    EXPECT_EQ(Field(answered, "yaml"), "ProcessName: extra\nExecutablePath: /bin/true\nArgs:\n  - \"8080\"\n");
    EXPECT_EQ(Ask(*params, {"show", "name: nobody\n"}).status, service_registry::ReplyStatus::Refused);
}

TEST(ProcessManagerTest, RegistryShowOfABrokenFileGivesItsText)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", auto_main);
    folder.Write("services.d/broken.yaml", "ProcessName: other\n");
    std::unique_ptr<process_manager::ProcessManagerParams> params = StartedWithProblems(folder);
    const Answered answered = Ask(*params, {"show", "name: broken\n"});
    ASSERT_EQ(answered.status, service_registry::ReplyStatus::Ok) << answered.json;
    EXPECT_EQ(Field(answered, "fileText"), "ProcessName: other\n");
    EXPECT_FALSE(Field(answered, "problem").empty());
}

TEST(ProcessManagerTest, RegistryRequestThatIsNotUnderstoodIsAnError)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", auto_main);
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    const Answered answered = Ask(*params, {"frobnicate"});
    EXPECT_EQ(answered.status, service_registry::ReplyStatus::Error);
    EXPECT_FALSE(Field(answered, "message").empty());
}

TEST(ProcessManagerTest, RegistryVersionAnswersTheProtocolVersion)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", auto_main);
    std::unique_ptr<process_manager::ProcessManagerParams> params = Started(folder);
    const Answered answered = Ask(*params, {"version"});
    ASSERT_EQ(answered.status, service_registry::ReplyStatus::Ok);
    EXPECT_EQ(Field(answered, "version"), std::to_string(service_registry::protocol_version));
}

TEST(ProcessManagerTest, RegistryWithoutASourceIsRefused)
{
    process_manager::ProcessManagerParams params{};
    std::vector<process_manager::Process> retiring;
    const service_registry::Reply reply = process_manager::ProcessManager::HandleRegistry(
        params, retiring, {"add", "name: extra\ndefinition: {ExecutablePath: /bin/true}\n"});
    EXPECT_EQ(reply.status, service_registry::ReplyStatus::Refused);
}

TEST(ProcessManagerTest, RemoteRegistrationIsReadFromTheManagerSection)
{
    std::vector<std::string> problems;
    config_section::ConfigValue document{};
    std::string error;
    ASSERT_EQ(config_section::ParseConfigText("ProcessManager:\n  RemoteRegistration: true\nServices: []\n", document, error),
              config_section::LoadCode::Ok);
    EXPECT_TRUE(process_manager::ProcessManager::GenerateConfig(document, &problems)->remoteRegistration);
    EXPECT_TRUE(problems.empty());
    ASSERT_EQ(config_section::ParseConfigText("Services: []\n", document, error), config_section::LoadCode::Ok);
    EXPECT_FALSE(process_manager::ProcessManager::GenerateConfig(document, &problems)->remoteRegistration);
}
