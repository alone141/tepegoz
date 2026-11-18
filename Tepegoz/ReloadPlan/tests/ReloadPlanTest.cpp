#include <gtest/gtest.h>

#include "ReloadPlan.hpp"

#include "ConfigSection.hpp"
#include "ConfigSource.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

using Names = std::vector<std::string>;

// A folder of its own for one test, removed with the test.
class ScratchFolder
{
public:
    ScratchFolder()
    {
        char pattern[] = "/tmp/reload_plan_test_XXXXXX";
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

    void Delete(const std::string& name) const
    {
        std::filesystem::remove(path / name);
    }

    std::filesystem::path File(const std::string& name) const
    {
        return path / name;
    }

    config_source::LoadedConfig Load() const
    {
        const config_source::ConfigSource source{path / "main.yaml"};
        config_source::LoadedConfig loaded{};
        std::string error;
        EXPECT_EQ(source.Load(loaded, error), config_source::LoadCode::Ok) << error;
        return loaded;
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

std::string Entry(const std::string& name, const std::string& more = "")
{
    return "  - ProcessName: " + name + "\n    ExecutablePath: /bin/true\n" + more;
}

std::string Service(const std::string& name, const std::string& more = "")
{
    return "ProcessName: " + name + "\nExecutablePath: /bin/true\n" + more;
}

std::string MainFile(const std::string& entries)
{
    return "Tepegoz:\n  Mode: Auto\nServices:\n" + entries;
}

config_section::ConfigValue Parse(const std::string& text)
{
    config_section::ConfigValue value{};
    std::string error;
    EXPECT_EQ(config_section::ParseConfigText(text, value, error), config_section::LoadCode::Ok) << error;
    return value;
}

// Plans a reload from the configuration as it was first written to the one on disk now.
reload_plan::ReloadPlan Plan(const config_section::ConfigValue& before, const ScratchFolder& folder, const Names& running,
                             const Names& built)
{
    reload_plan::ReloadPlan plan{};
    EXPECT_EQ(reload_plan::PlanReload(running, before, folder.Load(), built, plan), reload_plan::PlanCode::Ok);
    return plan;
}

reload_plan::PlanCode Refusal(const config_section::ConfigValue& before, const ScratchFolder& folder,
                              const Names& running, const Names& built)
{
    reload_plan::ReloadPlan plan{};
    plan.kept.push_back("untouched");
    const reload_plan::PlanCode code = reload_plan::PlanReload(running, before, folder.Load(), built, plan);
    EXPECT_EQ(plan.kept, Names{"untouched"});
    return code;
}

} // namespace

TEST(ReloadPlanTest, NewFileIsAdded)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("services.d/extra.yaml", Service("extra"));
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main"}, {"main", "extra"});
    EXPECT_EQ(plan.added, Names{"extra"});
    EXPECT_EQ(plan.unchanged, Names{"main"});
    EXPECT_TRUE(plan.removed.empty());
}

TEST(ReloadPlanTest, AddedServicesKeepTheStartOrder)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("services.d/alpha.yaml", Service("alpha", "DependsOn: [beta]\n"));
    folder.Write("services.d/beta.yaml", Service("beta"));
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main"}, {"main", "beta", "alpha"});
    EXPECT_EQ(plan.added, (Names{"beta", "alpha"}));
}

TEST(ReloadPlanTest, DeletedFileIsRemoved)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    folder.Write("services.d/extra.yaml", Service("extra"));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Delete("services.d/extra.yaml");
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main", "extra"}, {"main"});
    EXPECT_EQ(plan.removed, Names{"extra"});
    EXPECT_EQ(plan.unchanged, Names{"main"});
}

TEST(ReloadPlanTest, DeletedMainFileEntryIsRemoved)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main") + Entry("other")));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("main.yaml", MainFile(Entry("main")));
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main", "other"}, {"main"});
    EXPECT_EQ(plan.removed, Names{"other"});
}

TEST(ReloadPlanTest, EmptiedFolderRemovesItsServices)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    folder.Write("services.d/extra.yaml", Service("extra"));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Delete("services.d/extra.yaml");
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main", "extra"}, {"main"});
    EXPECT_EQ(plan.removed, Names{"extra"});
}

TEST(ReloadPlanTest, ServiceSwitchedOffIsRemoved)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    folder.Write("services.d/extra.yaml", Service("extra"));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("services.d/extra.yaml", Service("extra", "Enabled: false\n"));
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main", "extra"}, {"main"});
    EXPECT_EQ(plan.removed, Names{"extra"});
    EXPECT_TRUE(plan.kept.empty());
}

TEST(ReloadPlanTest, EditedServiceIsChanged)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    folder.Write("services.d/extra.yaml", Service("extra", "Args: [one]\n"));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("services.d/extra.yaml", Service("extra", "Args: [two]\n"));
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main", "extra"}, {"main", "extra"});
    EXPECT_EQ(plan.changed, Names{"extra"});
    EXPECT_EQ(plan.unchanged, Names{"main"});
}

TEST(ReloadPlanTest, AddedKeyIsAChange)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("main.yaml", MainFile(Entry("main", "    RestartMode: Always\n")));
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main"}, {"main"});
    EXPECT_EQ(plan.changed, Names{"main"});
}

TEST(ReloadPlanTest, SameServiceIsUnchanged)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    folder.Write("services.d/extra.yaml", Service("extra"));
    const config_section::ConfigValue before = folder.Load().document;
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main", "extra"}, {"main", "extra"});
    EXPECT_EQ(plan.unchanged, (Names{"main", "extra"}));
    EXPECT_TRUE(plan.added.empty());
    EXPECT_TRUE(plan.removed.empty());
    EXPECT_TRUE(plan.changed.empty());
    EXPECT_TRUE(plan.kept.empty());
    EXPECT_TRUE(plan.skipped.empty());
}

TEST(ReloadPlanTest, CommentsKeyOrderAndLayoutAreNoChange)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    folder.Write("services.d/extra.yaml", Service("extra", "Args: [--port, 1001]\nGracePeriodMs: 500\n"));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("services.d/extra.yaml", "# the extra service\n"
                                          "GracePeriodMs:   500\n"
                                          "Args:\n"
                                          "  - --port\n"
                                          "  - 1001   # its port\n"
                                          "ExecutablePath: /bin/true\n"
                                          "ProcessName: extra\n");
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main", "extra"}, {"main", "extra"});
    EXPECT_EQ(plan.unchanged, (Names{"main", "extra"}));
    EXPECT_TRUE(plan.changed.empty());
}

TEST(ReloadPlanTest, ServiceMovedToAFileWithTheSameDefinitionIsUnchanged)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main") + Entry("extra", "    Args: [one]\n")));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("main.yaml", MainFile(Entry("main")));
    folder.Write("services.d/extra.yaml", Service("extra", "Args: [one]\n"));
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main", "extra"}, {"main", "extra"});
    EXPECT_EQ(plan.unchanged, (Names{"main", "extra"}));
}

TEST(ReloadPlanTest, ChangedManagerModeIsNoChangeOfTheServices)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("main.yaml", "Tepegoz:\n  Mode: Manual\nServices:\n" + Entry("main"));
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main"}, {"main"});
    EXPECT_EQ(plan.unchanged, Names{"main"});
}

TEST(ReloadPlanTest, ServiceWithoutAnOldDefinitionIsChanged)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("services.d/extra.yaml", Service("extra"));
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main", "extra"}, {"main", "extra"});
    EXPECT_EQ(plan.changed, Names{"extra"});
}

TEST(ReloadPlanTest, ServiceDisabledAtRunTimeIsAddedAgain)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main") + Entry("other")));
    const config_section::ConfigValue before = folder.Load().document;
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main"}, {"main", "other"});
    EXPECT_EQ(plan.added, Names{"other"});
}

TEST(ReloadPlanTest, BrokenFileOfARunningServiceIsKept)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    folder.Write("services.d/extra.yaml", Service("extra"));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("services.d/extra.yaml", "ProcessName: extra\n  ExecutablePath: [\n");
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main", "extra"}, {"main"});
    EXPECT_EQ(plan.kept, Names{"extra"});
    EXPECT_TRUE(plan.removed.empty());
}

TEST(ReloadPlanTest, FileThatNamesAnotherServiceKeepsItsOwn)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    folder.Write("services.d/extra.yaml", Service("extra"));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("services.d/extra.yaml", Service("other"));
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main", "extra"}, {"main"});
    EXPECT_EQ(plan.kept, Names{"extra"});
}

TEST(ReloadPlanTest, FileThatCannotBeReadKeepsItsService)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    folder.Write("services.d/extra.yaml", Service("extra"));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Delete("services.d/extra.yaml");
    std::filesystem::create_symlink("/nonexistent/reload_plan_test.yaml", folder.File("services.d/extra.yaml"));
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main", "extra"}, {"main"});
    EXPECT_EQ(plan.kept, Names{"extra"});
}

TEST(ReloadPlanTest, EntryTheManagerCannotBuildIsKept)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main") + Entry("other")));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("main.yaml", MainFile(Entry("main") + "  - ProcessName: other\n"));
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main", "other"}, {"main"});
    EXPECT_EQ(plan.kept, Names{"other"});
}

TEST(ReloadPlanTest, BrokenFileOfANewServiceIsSkipped)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("services.d/extra.yaml", "ProcessName: [extra\n");
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main"}, {"main"});
    EXPECT_EQ(plan.skipped, Names{"extra"});
    EXPECT_TRUE(plan.added.empty());
    EXPECT_TRUE(plan.kept.empty());
}

TEST(ReloadPlanTest, FileOfAServiceTheMainFileDefinesIsNotAProblemOfThatService)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("services.d/main.yaml", Service("main", "Args: [other]\n"));
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main"}, {"main"});
    EXPECT_EQ(plan.unchanged, Names{"main"});
    EXPECT_TRUE(plan.kept.empty());
    EXPECT_TRUE(plan.skipped.empty());
}

TEST(ReloadPlanTest, FolderThatCannotBeReadRefusesTheReload)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    folder.Write("services.d/extra.yaml", Service("extra"));
    const config_section::ConfigValue before = folder.Load().document;
    std::filesystem::remove_all(folder.File("services.d"));
    // A link to itself: asking what it is fails.
    std::filesystem::create_symlink("services.d", folder.File("services.d"));
    EXPECT_EQ(Refusal(before, folder, {"main", "extra"}, {"main"}), reload_plan::PlanCode::FolderNotReadable);
}

TEST(ReloadPlanTest, FolderThatIsAFileRefusesTheReload)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    folder.Write("services.d/extra.yaml", Service("extra"));
    const config_section::ConfigValue before = folder.Load().document;
    std::filesystem::remove_all(folder.File("services.d"));
    folder.Write("services.d", "not a folder\n");
    EXPECT_EQ(Refusal(before, folder, {"main", "extra"}, {"main"}), reload_plan::PlanCode::FolderNotReadable);
}

TEST(ReloadPlanTest, ServicesThatIsNotAListRefusesTheReload)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("main.yaml", "Services:\n  ProcessName: main\n");
    EXPECT_EQ(Refusal(before, folder, {"main"}, {}), reload_plan::PlanCode::NoServicesList);
}

TEST(ReloadPlanTest, TopLevelThatIsNotAMappingRefusesTheReload)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("main.yaml", "- ProcessName: main\n");
    EXPECT_EQ(Refusal(before, folder, {"main"}, {}), reload_plan::PlanCode::NoServicesList);
}

TEST(ReloadPlanTest, DuplicateNameRefusesTheReload)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("main.yaml", MainFile(Entry("main") + Entry("main")));
    EXPECT_EQ(Refusal(before, folder, {"main"}, {"main", "main"}), reload_plan::PlanCode::DuplicateName);
}

TEST(ReloadPlanTest, FindDefinitionSkipsADisabledEntry)
{
    const config_section::ConfigValue document =
        Parse(MainFile(Entry("main", "    Enabled: false\n    Args: [off]\n") + Entry("main", "    Args: [on]\n")));
    const config_section::ConfigValue* found = reload_plan::FindDefinition(document, "main");
    ASSERT_NE(found, nullptr);
    const config_section::ConfigValue* args = config_section::FindValue(*found, "Args");
    ASSERT_NE(args, nullptr);
    ASSERT_EQ(args->children.size(), 1U);
    EXPECT_EQ(args->children[0].text, "on");
}

TEST(ReloadPlanTest, FindDefinitionWithoutTheServiceIsNull)
{
    EXPECT_EQ(reload_plan::FindDefinition(Parse(MainFile(Entry("main"))), "other"), nullptr);
}

TEST(ReloadPlanTest, SameDefinitionIgnoresKeyOrder)
{
    EXPECT_TRUE(reload_plan::SameDefinition(Parse("a: 1\nb: [x, y]\n"), Parse("b: [x, y]\na: 1\n")));
}

TEST(ReloadPlanTest, SameDefinitionSeesAChangedValue)
{
    EXPECT_FALSE(reload_plan::SameDefinition(Parse("a: 1\nb: [x, y]\n"), Parse("a: 1\nb: [y, x]\n")));
}

TEST(ReloadPlanTest, SameDefinitionSeesAMissingKey)
{
    EXPECT_FALSE(reload_plan::SameDefinition(Parse("a: 1\nb: 2\n"), Parse("a: 1\n")));
}

TEST(ReloadPlanTest, EntryWithoutAReadableNameRemovesNothing)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main")));
    folder.Write("services.d/extra.yaml", Service("extra"));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("main.yaml", MainFile("  - ProcessName: [main]\n    ExecutablePath: /bin/true\n"));
    folder.Delete("services.d/extra.yaml");
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main", "extra"}, {});
    EXPECT_TRUE(plan.unnamedEntry);
    EXPECT_EQ(plan.kept, (Names{"main", "extra"}));
    EXPECT_TRUE(plan.removed.empty());
}

TEST(ReloadPlanTest, ServiceSwitchedOffIsRemovedBesideAnEntryWithoutAName)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main") + Entry("other")));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("main.yaml", MainFile(Entry("main") + Entry("other", "    Enabled: false\n") +
                                       "  - ExecutablePath: /bin/true\n"));
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main", "other"}, {"main"});
    EXPECT_TRUE(plan.unnamedEntry);
    EXPECT_EQ(plan.removed, Names{"other"});
}

TEST(ReloadPlanTest, DisabledEntryWithoutANameIsNoUnnamedEntry)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", MainFile(Entry("main") + Entry("other")));
    const config_section::ConfigValue before = folder.Load().document;
    folder.Write("main.yaml", MainFile(Entry("main") + "  - Enabled: false\n"));
    const reload_plan::ReloadPlan plan = Plan(before, folder, {"main", "other"}, {"main"});
    EXPECT_FALSE(plan.unnamedEntry);
    EXPECT_EQ(plan.removed, Names{"other"});
}
