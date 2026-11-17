#include <gtest/gtest.h>

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

const std::string main_text = "ProcessManager:\n"
                              "  Mode: Auto\n"
                              "Services:\n"
                              "  - ProcessName: main\n"
                              "    ExecutablePath: /bin/true\n";

// A folder of its own for one test, removed with the test.
class ScratchFolder
{
public:
    ScratchFolder()
    {
        char pattern[] = "/tmp/config_source_test_XXXXXX";
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

    std::filesystem::path DropIn(const std::string& name) const
    {
        return path / "services.d" / name;
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
    return loaded;
}

// The ProcessName of every item of the merged Services list, in order.
std::vector<std::string> ServiceNames(const config_source::LoadedConfig& loaded)
{
    std::vector<std::string> names;
    const config_section::ConfigValue* services = config_section::FindValue(loaded.document, "Services");
    if (services == nullptr)
    {
        return names;
    }

    for (const config_section::ConfigValue& item : services->children)
    {
        const config_section::ConfigValue* name = config_section::FindValue(item, "ProcessName");
        names.push_back(name != nullptr ? name->text : std::string{});
    }

    return names;
}

bool HasProblemWith(const config_source::LoadedConfig& loaded, const std::string& part)
{
    for (const std::string& problem : loaded.problems)
    {
        if (problem.find(part) != std::string::npos)
        {
            return true;
        }
    }

    return false;
}

std::string Service(const std::string& name)
{
    return "ProcessName: " + name + "\nExecutablePath: /bin/true\n";
}

} // namespace

TEST(ConfigSourceTest, NoFolderLoadsTheMainFileAlone)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", main_text);
    const config_source::LoadedConfig loaded = Load(folder);
    EXPECT_EQ(loaded.mainFile, folder.MainFile());
    EXPECT_EQ(ServiceNames(loaded), (std::vector<std::string>{"main"}));
    ASSERT_EQ(loaded.origins.size(), 1U);
    EXPECT_EQ(loaded.origins[0].processName, "main");
    EXPECT_EQ(loaded.origins[0].kind, config_source::OriginKind::MainFile);
    EXPECT_EQ(loaded.origins[0].file, folder.MainFile());
    EXPECT_TRUE(loaded.dropIns.empty());
    EXPECT_TRUE(loaded.problems.empty());
}

TEST(ConfigSourceTest, DropInServiceIsAppendedWithItsOrigin)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", main_text);
    folder.Write("services.d/extra.yaml", Service("extra") + "Args: [--port, 1001]\n");
    const config_source::LoadedConfig loaded = Load(folder);
    EXPECT_EQ(ServiceNames(loaded), (std::vector<std::string>{"main", "extra"}));
    ASSERT_EQ(loaded.origins.size(), 2U);
    EXPECT_EQ(loaded.origins[1].processName, "extra");
    EXPECT_EQ(loaded.origins[1].kind, config_source::OriginKind::DropIn);
    EXPECT_EQ(loaded.origins[1].file, folder.DropIn("extra.yaml"));
    ASSERT_EQ(loaded.dropIns.size(), 1U);
    EXPECT_EQ(loaded.dropIns[0].serviceName, "extra");
    EXPECT_EQ(loaded.dropIns[0].code, config_source::DropInCode::Ok);
    EXPECT_TRUE(loaded.problems.empty());
}

TEST(ConfigSourceTest, DropInKeepsEveryKeyOfItsService)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", main_text);
    folder.Write("services.d/extra.yaml", Service("extra") + "Args: [--port, 1001]\nDependsOn: [main]\n");
    const config_source::LoadedConfig loaded = Load(folder);
    const config_section::ConfigValue* services = config_section::FindValue(loaded.document, "Services");
    ASSERT_NE(services, nullptr);
    ASSERT_EQ(services->children.size(), 2U);
    const config_section::ConfigValue* args = config_section::FindValue(services->children[1], "Args");
    ASSERT_NE(args, nullptr);
    ASSERT_EQ(args->children.size(), 2U);
    EXPECT_EQ(args->children[1].text, "1001");
    EXPECT_NE(config_section::FindValue(services->children[1], "DependsOn"), nullptr);
}

TEST(ConfigSourceTest, DropInsFollowTheMainServicesInSortedFileNameOrder)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", main_text);
    folder.Write("services.d/zeta.yaml", Service("zeta"));
    folder.Write("services.d/alpha.yaml", Service("alpha"));
    folder.Write("services.d/mid.yaml", Service("mid"));
    const config_source::LoadedConfig loaded = Load(folder);
    EXPECT_EQ(ServiceNames(loaded), (std::vector<std::string>{"main", "alpha", "mid", "zeta"}));
    ASSERT_EQ(loaded.dropIns.size(), 3U);
    EXPECT_EQ(loaded.dropIns[0].serviceName, "alpha");
    EXPECT_EQ(loaded.dropIns[2].serviceName, "zeta");
}

TEST(ConfigSourceTest, ProcessNameThatDiffersFromTheFileNameSkipsTheFile)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", main_text);
    folder.Write("services.d/extra.yaml", Service("other"));
    const config_source::LoadedConfig loaded = Load(folder);
    EXPECT_EQ(ServiceNames(loaded), (std::vector<std::string>{"main"}));
    ASSERT_EQ(loaded.dropIns.size(), 1U);
    EXPECT_EQ(loaded.dropIns[0].code, config_source::DropInCode::NameMismatch);
    EXPECT_TRUE(HasProblemWith(loaded, folder.DropIn("extra.yaml").string() +
                                           ": ProcessName is 'other', but the file name says 'extra'; file skipped"));
}

TEST(ConfigSourceTest, MissingProcessNameSkipsTheFile)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", main_text);
    folder.Write("services.d/extra.yaml", "ExecutablePath: /bin/true\n");
    const config_source::LoadedConfig loaded = Load(folder);
    EXPECT_EQ(ServiceNames(loaded), (std::vector<std::string>{"main"}));
    ASSERT_EQ(loaded.dropIns.size(), 1U);
    EXPECT_EQ(loaded.dropIns[0].code, config_source::DropInCode::NameMismatch);
    EXPECT_TRUE(HasProblemWith(loaded, "ProcessName is missing; the file name says 'extra'; file skipped"));
}

TEST(ConfigSourceTest, NameTheMainFileDefinesSkipsTheFile)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", main_text);
    folder.Write("services.d/main.yaml", Service("main"));
    const config_source::LoadedConfig loaded = Load(folder);
    EXPECT_EQ(ServiceNames(loaded), (std::vector<std::string>{"main"}));
    ASSERT_EQ(loaded.origins.size(), 1U);
    EXPECT_EQ(loaded.origins[0].kind, config_source::OriginKind::MainFile);
    ASSERT_EQ(loaded.dropIns.size(), 1U);
    EXPECT_EQ(loaded.dropIns[0].code, config_source::DropInCode::NameTaken);
    EXPECT_TRUE(HasProblemWith(loaded, "the main file already defines service 'main', which is used instead"));
}

TEST(ConfigSourceTest, NameOfADisabledMainServiceIsTakenToo)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", main_text + "  - ProcessName: idle\n    ExecutablePath: /bin/true\n    Enabled: false\n");
    folder.Write("services.d/idle.yaml", Service("idle"));
    const config_source::LoadedConfig loaded = Load(folder);
    ASSERT_EQ(loaded.dropIns.size(), 1U);
    EXPECT_EQ(loaded.dropIns[0].code, config_source::DropInCode::NameTaken);
}

TEST(ConfigSourceTest, InvalidYamlSkipsTheFileAndKeepsTheOthers)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", main_text);
    folder.Write("services.d/broken.yaml", "ProcessName: broken\n  ExecutablePath: [\n");
    folder.Write("services.d/good.yaml", Service("good"));
    const config_source::LoadedConfig loaded = Load(folder);
    EXPECT_EQ(ServiceNames(loaded), (std::vector<std::string>{"main", "good"}));
    ASSERT_EQ(loaded.dropIns.size(), 2U);
    EXPECT_EQ(loaded.dropIns[0].code, config_source::DropInCode::SyntaxError);
    EXPECT_EQ(loaded.dropIns[1].code, config_source::DropInCode::Ok);
    EXPECT_TRUE(HasProblemWith(loaded, folder.DropIn("broken.yaml").string() + ": not valid YAML: line "));
    EXPECT_EQ(loaded.problems.size(), 1U);
}

TEST(ConfigSourceTest, TopLevelThatIsNotAMappingSkipsTheFile)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", main_text);
    folder.Write("services.d/list.yaml", "- ProcessName: list\n  ExecutablePath: /bin/true\n");
    const config_source::LoadedConfig loaded = Load(folder);
    EXPECT_EQ(ServiceNames(loaded), (std::vector<std::string>{"main"}));
    ASSERT_EQ(loaded.dropIns.size(), 1U);
    EXPECT_EQ(loaded.dropIns[0].code, config_source::DropInCode::NotMapping);
    EXPECT_TRUE(HasProblemWith(loaded, "list.yaml: must be a mapping of keys to values"));
}

TEST(ConfigSourceTest, EmptyDropInIsNotAMapping)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", main_text);
    folder.Write("services.d/empty.yaml", "# nothing yet\n");
    const config_source::LoadedConfig loaded = Load(folder);
    ASSERT_EQ(loaded.dropIns.size(), 1U);
    EXPECT_EQ(loaded.dropIns[0].code, config_source::DropInCode::NotMapping);
}

TEST(ConfigSourceTest, DropInThatCannotBeReadSkipsTheFile)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", main_text);
    folder.Write("services.d/good.yaml", Service("good"));
    std::filesystem::create_symlink("/nonexistent/config_source_test.yaml", folder.DropIn("ghost.yaml"));
    const config_source::LoadedConfig loaded = Load(folder);
    EXPECT_EQ(ServiceNames(loaded), (std::vector<std::string>{"main", "good"}));
    ASSERT_EQ(loaded.dropIns.size(), 2U);
    EXPECT_EQ(loaded.dropIns[0].code, config_source::DropInCode::NotReadable);
    EXPECT_TRUE(HasProblemWith(loaded, "ghost.yaml: cannot be read; file skipped"));
}

TEST(ConfigSourceTest, DotFilesOtherExtensionsAndFoldersAreIgnoredSilently)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", main_text);
    folder.Write("services.d/.hidden.yaml", "not: [valid\n");
    folder.Write("services.d/.writing.yaml.tmp", "not: [valid\n");
    folder.Write("services.d/notes.txt", "not: [valid\n");
    folder.Write("services.d/extra.yaml.bak", "not: [valid\n");
    folder.Write("services.d/UPPER.YAML", "not: [valid\n");
    folder.Write("services.d/folder.yaml/inner.yaml", "not: [valid\n");
    folder.Write("services.d/sub/inner.yaml", "not: [valid\n");
    const config_source::LoadedConfig loaded = Load(folder);
    EXPECT_EQ(ServiceNames(loaded), (std::vector<std::string>{"main"}));
    EXPECT_TRUE(loaded.dropIns.empty());
    EXPECT_TRUE(loaded.problems.empty());
}

TEST(ConfigSourceTest, DisabledDropInIsAddedForTheReaderToSkip)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", main_text);
    folder.Write("services.d/paused.yaml", Service("paused") + "Enabled: false\n");
    const config_source::LoadedConfig loaded = Load(folder);
    EXPECT_EQ(ServiceNames(loaded), (std::vector<std::string>{"main", "paused"}));
    ASSERT_EQ(loaded.dropIns.size(), 1U);
    EXPECT_EQ(loaded.dropIns[0].code, config_source::DropInCode::Ok);
    EXPECT_TRUE(loaded.problems.empty());
}

TEST(ConfigSourceTest, MissingMainFileReturnsMainFileNotOpened)
{
    const ScratchFolder folder{};
    folder.Write("services.d/extra.yaml", Service("extra"));
    const config_source::ConfigSource source{folder.MainFile()};
    config_source::LoadedConfig loaded{};
    loaded.problems.push_back("untouched");
    std::string error;
    EXPECT_EQ(source.Load(loaded, error), config_source::LoadCode::MainFileNotOpened);
    EXPECT_NE(error.find(folder.MainFile().string()), std::string::npos) << error;
    EXPECT_EQ(loaded.problems, (std::vector<std::string>{"untouched"}));
}

TEST(ConfigSourceTest, MainFileSyntaxErrorReturnsMainFileSyntaxError)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "a:\n  b: 1\n c: 2\n");
    folder.Write("services.d/extra.yaml", Service("extra"));
    const config_source::ConfigSource source{folder.MainFile()};
    config_source::LoadedConfig loaded{};
    std::string error;
    EXPECT_EQ(source.Load(loaded, error), config_source::LoadCode::MainFileSyntaxError);
    EXPECT_EQ(error.rfind("line 3, column 2: ", 0), 0U) << error;
    EXPECT_TRUE(loaded.origins.empty());
}

TEST(ConfigSourceTest, MainFileWithoutServicesGetsTheList)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "ProcessManager:\n  Mode: Auto\n");
    folder.Write("services.d/extra.yaml", Service("extra"));
    const config_source::LoadedConfig loaded = Load(folder);
    EXPECT_EQ(ServiceNames(loaded), (std::vector<std::string>{"extra"}));
    EXPECT_EQ(loaded.document.keys, (std::vector<std::string>{"ProcessManager", "Services"}));
    EXPECT_NE(config_section::FindValue(loaded.document, "ProcessManager"), nullptr);
}

TEST(ConfigSourceTest, EmptyMainFileGetsTheList)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "# no services in the main file\n");
    folder.Write("services.d/extra.yaml", Service("extra"));
    const config_source::LoadedConfig loaded = Load(folder);
    EXPECT_EQ(loaded.document.kind, config_section::ValueKind::Mapping);
    EXPECT_EQ(ServiceNames(loaded), (std::vector<std::string>{"extra"}));
}

TEST(ConfigSourceTest, MainFileWithoutDropInsIsLeftAsItIs)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "# no services in the main file\n");
    const config_source::LoadedConfig loaded = Load(folder);
    EXPECT_EQ(loaded.document.kind, config_section::ValueKind::Null);
}

TEST(ConfigSourceTest, MainServicesThatIsNotAListLeavesTheFolderUnread)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", "Services:\n  ProcessName: main\n");
    folder.Write("services.d/extra.yaml", Service("extra"));
    const config_source::LoadedConfig loaded = Load(folder);
    const config_section::ConfigValue* services = config_section::FindValue(loaded.document, "Services");
    ASSERT_NE(services, nullptr);
    EXPECT_EQ(services->kind, config_section::ValueKind::Mapping);
    EXPECT_TRUE(loaded.origins.empty());
    EXPECT_TRUE(loaded.dropIns.empty());
    EXPECT_TRUE(HasProblemWith(loaded, "no registered service is read, because the main file has no Services list"));
}

TEST(ConfigSourceTest, FolderThatIsAFileIsReported)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", main_text);
    folder.Write("services.d", "not a folder\n");
    const config_source::LoadedConfig loaded = Load(folder);
    EXPECT_EQ(ServiceNames(loaded), (std::vector<std::string>{"main"}));
    EXPECT_TRUE(HasProblemWith(loaded, "services.d: is not a folder; no registered service is read"));
}

TEST(ConfigSourceTest, LoadingAgainSeesTheFolderAsItIsNow)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", main_text);
    folder.Write("services.d/first.yaml", Service("first"));
    const config_source::ConfigSource source{folder.MainFile()};
    config_source::LoadedConfig loaded{};
    std::string error;
    ASSERT_EQ(source.Load(loaded, error), config_source::LoadCode::Ok) << error;
    EXPECT_EQ(ServiceNames(loaded), (std::vector<std::string>{"main", "first"}));

    std::filesystem::remove(folder.DropIn("first.yaml"));
    folder.Write("services.d/second.yaml", Service("second"));
    ASSERT_EQ(source.Load(loaded, error), config_source::LoadCode::Ok) << error;
    EXPECT_EQ(ServiceNames(loaded), (std::vector<std::string>{"main", "second"}));
    ASSERT_EQ(loaded.dropIns.size(), 1U);
    EXPECT_EQ(loaded.dropIns[0].serviceName, "second");
}

TEST(ConfigSourceTest, DropInFolderIsBesideTheMainFile)
{
    const config_source::ConfigSource source{"/etc/processmanager/ProcessManagerConfig.yaml"};
    EXPECT_EQ(source.MainFile(), std::filesystem::path{"/etc/processmanager/ProcessManagerConfig.yaml"});
    EXPECT_EQ(source.DropInFolder(), std::filesystem::path{"/etc/processmanager/services.d"});
}

TEST(ConfigSourceTest, FindOriginFindsTheServiceByName)
{
    const ScratchFolder folder{};
    folder.Write("main.yaml", main_text);
    folder.Write("services.d/extra.yaml", Service("extra"));
    const config_source::LoadedConfig loaded = Load(folder);
    const config_source::ServiceOrigin* extra = config_source::FindOrigin(loaded.origins, "extra");
    ASSERT_NE(extra, nullptr);
    EXPECT_EQ(extra->kind, config_source::OriginKind::DropIn);
    EXPECT_EQ(extra->file, folder.DropIn("extra.yaml"));
    const config_source::ServiceOrigin* mainOrigin = config_source::FindOrigin(loaded.origins, "main");
    ASSERT_NE(mainOrigin, nullptr);
    EXPECT_EQ(mainOrigin->kind, config_source::OriginKind::MainFile);
    EXPECT_EQ(config_source::FindOrigin(loaded.origins, "unknown"), nullptr);
}
