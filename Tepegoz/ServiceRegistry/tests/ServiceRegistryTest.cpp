#include <gtest/gtest.h>

#include "ServiceRegistry.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>
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
        char pattern[] = "/tmp/service_registry_test_XXXXXX";
        const char* made = ::mkdtemp(pattern);
        path = made != nullptr ? std::filesystem::path{made} : std::filesystem::path{};
    }

    ~ScratchFolder()
    {
        if (!path.empty())
        {
            std::error_code ignored{};
            std::filesystem::remove_all(path, ignored);
        }
    }

    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;

    std::filesystem::path Path() const
    {
        return path;
    }

private:
    std::filesystem::path path;
};

std::string Contents(const std::filesystem::path& file)
{
    std::ifstream stream{file, std::ios::binary};
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

config_section::ConfigValue Parse(const std::string& text)
{
    config_section::ConfigValue value{};
    std::string error;
    EXPECT_EQ(config_section::ParseConfigText(text, value, error), config_section::LoadCode::Ok) << error << "\n"
                                                                                                 << text;
    return value;
}

bool Same(const config_section::ConfigValue& a, const config_section::ConfigValue& b)
{
    if (a.kind != b.kind || a.text != b.text || a.keys != b.keys || a.children.size() != b.children.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < a.children.size(); ++i)
    {
        if (!Same(a.children[i], b.children[i]))
        {
            return false;
        }
    }

    return true;
}

config_section::ConfigValue Text(const std::string& text)
{
    config_section::ConfigValue value{};
    value.kind = config_section::ValueKind::String;
    value.text = text;
    return value;
}

service_registry::RequestCode ParseFrames(const std::vector<std::string>& frames, service_registry::Request& out)
{
    std::string problem;
    const service_registry::RequestCode code = service_registry::ParseRequest(frames, out, problem);
    if (code != service_registry::RequestCode::Ok)
    {
        EXPECT_FALSE(problem.empty());
    }

    return code;
}

const std::string rich_definition = "ProcessName: cam\n"
                                    "ExecutablePath: /opt/cam/bin/server\n"
                                    "Args:\n"
                                    "  - \"--port\"\n"
                                    "  - \"8080\"\n"
                                    "  - \"a: b\"\n"
                                    "  - \"# not a comment\"\n"
                                    "  - \"  leading\"\n"
                                    "  - \"\"\n"
                                    "  - \"true\"\n"
                                    "  - \"null\"\n"
                                    "  - \"~\"\n"
                                    "  - \"1.5\"\n"
                                    "  - \"a\\\"quote\"\n"
                                    "  - \"back\\\\slash\"\n"
                                    "  - \"tab\\there\"\n"
                                    "  - \"line\\nbreak\"\n"
                                    "  - \"\xc3\xbc" "nicode\"\n"
                                    "  - \"x  y\"\n"
                                    "  - \"-\"\n"
                                    "  - plain/text\n"
                                    "EnvironmentVariables: {PYTHONPATH: /usr/lib, \"WEIRD KEY\": x, \"1\": one, \"true\": t}\n"
                                    "Display: \":1\"\n"
                                    "MaxRestarts: 3\n"
                                    "BackoffMultiplier: 2.0\n"
                                    "Ratio: 0.1\n"
                                    "Huge: 1.0e+20\n"
                                    "Negative: -3\n"
                                    "CheckHealth: false\n"
                                    "Cwd: null\n"
                                    "DependsOn: []\n"
                                    "Empty: {}\n"
                                    "Nested: [[1, \"2\"], {a: b}]\n";

} // namespace

TEST(ServiceRegistryTest, ParsesAnAddWithItsDefinitionAndFlags)
{
    service_registry::Request request{};
    ASSERT_EQ(ParseFrames({"add", "name: cam\ndefinition:\n  ExecutablePath: /opt/cam\nstart: false\ndryRun: true\n"},
                          request),
              service_registry::RequestCode::Ok);
    EXPECT_EQ(request.operation, service_registry::Operation::Add);
    EXPECT_EQ(request.name, "cam");
    ASSERT_NE(config_section::FindValue(request.definition, "ExecutablePath"), nullptr);
    EXPECT_FALSE(request.start);
    EXPECT_TRUE(request.dryRun);
    EXPECT_FALSE(request.restart);
}

TEST(ServiceRegistryTest, ParsesJsonParameters)
{
    service_registry::Request request{};
    const std::string json = "{\"name\": \"cam\", \"definition\": {\"ExecutablePath\": \"/opt/cam\", \"Args\": "
                             "[\"--port\", \"8080\"], \"MaxRestarts\": 3, \"BackoffMultiplier\": 2.5, "
                             "\"CheckHealth\": false, \"EnvironmentVariables\": {\"A\": \"1\"}}, \"start\": true}";
    ASSERT_EQ(ParseFrames({"add", json}, request), service_registry::RequestCode::Ok);
    const config_section::ConfigValue* args = config_section::FindValue(request.definition, "Args");
    ASSERT_NE(args, nullptr);
    ASSERT_EQ(args->children.size(), 2U);
    EXPECT_EQ(args->children[1].kind, config_section::ValueKind::String);
    EXPECT_EQ(args->children[1].text, "8080");
    const config_section::ConfigValue* restarts = config_section::FindValue(request.definition, "MaxRestarts");
    ASSERT_NE(restarts, nullptr);
    EXPECT_EQ(restarts->kind, config_section::ValueKind::Integer);
    const config_section::ConfigValue* health = config_section::FindValue(request.definition, "CheckHealth");
    ASSERT_NE(health, nullptr);
    EXPECT_EQ(health->kind, config_section::ValueKind::Boolean);
    EXPECT_EQ(health->text, "false");
}

TEST(ServiceRegistryTest, JsonDefinitionReadsLikeTheSameYaml)
{
    const config_section::ConfigValue fromJson =
        Parse("{\"ExecutablePath\": \"/opt/cam\", \"Args\": [\"-v\", \"2\"], \"MaxRestarts\": 3, "
              "\"BackoffMultiplier\": 2.5, \"CheckHealth\": true, \"Display\": \":1\", \"Cwd\": null, "
              "\"EnvironmentVariables\": {\"A\": \"x: y\"}}");
    const config_section::ConfigValue fromYaml = Parse("ExecutablePath: /opt/cam\n"
                                                       "Args: [\"-v\", \"2\"]\n"
                                                       "MaxRestarts: 3\n"
                                                       "BackoffMultiplier: 2.5\n"
                                                       "CheckHealth: true\n"
                                                       "Display: \":1\"\n"
                                                       "Cwd: null\n"
                                                       "EnvironmentVariables:\n"
                                                       "  A: \"x: y\"\n");
    EXPECT_TRUE(Same(fromJson, fromYaml));
}

TEST(ServiceRegistryTest, VersionAndListTakeNoParameters)
{
    service_registry::Request request{};
    EXPECT_EQ(ParseFrames({"version"}, request), service_registry::RequestCode::Ok);
    EXPECT_EQ(request.operation, service_registry::Operation::Version);
    EXPECT_EQ(ParseFrames({"list", "{}"}, request), service_registry::RequestCode::Ok);
    EXPECT_EQ(request.operation, service_registry::Operation::List);
    EXPECT_EQ(ParseFrames({"list", ""}, request), service_registry::RequestCode::Ok);
    EXPECT_EQ(ParseFrames({"list", "name: cam"}, request), service_registry::RequestCode::UnknownParameter);
}

TEST(ServiceRegistryTest, RequestWithoutAnOperationIsRefused)
{
    service_registry::Request request{};
    EXPECT_EQ(ParseFrames({}, request), service_registry::RequestCode::NoOperation);
    EXPECT_EQ(ParseFrames({""}, request), service_registry::RequestCode::NoOperation);
}

TEST(ServiceRegistryTest, UnknownOperationIsRefused)
{
    service_registry::Request request{};
    EXPECT_EQ(ParseFrames({"delete", "name: cam"}, request), service_registry::RequestCode::UnknownOperation);
}

TEST(ServiceRegistryTest, RequestOverTheLimitIsTooLarge)
{
    service_registry::Request request{};
    const std::string big = "name: cam\ndefinition:\n  Args: [\"" + std::string(service_registry::max_request_bytes, 'x') +
                            "\"]\n";
    EXPECT_EQ(ParseFrames({"add", big}, request), service_registry::RequestCode::TooLarge);
}

TEST(ServiceRegistryTest, ThreeFramesAreTooMany)
{
    service_registry::Request request{};
    EXPECT_EQ(ParseFrames({"show", "name: cam", "more"}, request), service_registry::RequestCode::TooManyFrames);
}

TEST(ServiceRegistryTest, ParametersThatAreNotYamlAreRefused)
{
    service_registry::Request request{};
    EXPECT_EQ(ParseFrames({"show", "name: \"cam"}, request), service_registry::RequestCode::NotYaml);
}

TEST(ServiceRegistryTest, ParametersThatAreNotAMappingAreRefused)
{
    service_registry::Request request{};
    EXPECT_EQ(ParseFrames({"show", "[cam]"}, request), service_registry::RequestCode::NotMapping);
}

TEST(ServiceRegistryTest, ParameterOfAnotherOperationIsRefused)
{
    service_registry::Request request{};
    EXPECT_EQ(ParseFrames({"remove", "name: cam\ndefinition: {ExecutablePath: /bin/true}"}, request),
              service_registry::RequestCode::UnknownParameter);
    EXPECT_EQ(ParseFrames({"add", "name: cam\ndefinition: {ExecutablePath: /bin/true}\nforce: true"}, request),
              service_registry::RequestCode::UnknownParameter);
}

TEST(ServiceRegistryTest, FlagThatIsNotABooleanIsRefused)
{
    service_registry::Request request{};
    EXPECT_EQ(ParseFrames({"add", "name: cam\ndefinition: {ExecutablePath: /bin/true}\nstart: \"no\""}, request),
              service_registry::RequestCode::WrongKind);
}

TEST(ServiceRegistryTest, DefinitionThatIsNotAMappingIsRefused)
{
    service_registry::Request request{};
    EXPECT_EQ(ParseFrames({"add", "name: cam\ndefinition: [ExecutablePath]"}, request),
              service_registry::RequestCode::WrongKind);
}

TEST(ServiceRegistryTest, MissingNameIsRefused)
{
    service_registry::Request request{};
    EXPECT_EQ(ParseFrames({"show"}, request), service_registry::RequestCode::MissingName);
    EXPECT_EQ(ParseFrames({"remove", "{}"}, request), service_registry::RequestCode::MissingName);
}

TEST(ServiceRegistryTest, AddWithoutADefinitionIsRefused)
{
    service_registry::Request request{};
    EXPECT_EQ(ParseFrames({"add", "name: cam"}, request), service_registry::RequestCode::MissingDefinition);
    EXPECT_EQ(ParseFrames({"replace", "name: cam"}, request), service_registry::RequestCode::MissingDefinition);
}

TEST(ServiceRegistryTest, UpdateWithoutAChangeIsRefused)
{
    service_registry::Request request{};
    EXPECT_EQ(ParseFrames({"update", "name: cam"}, request), service_registry::RequestCode::MissingChange);
    EXPECT_EQ(ParseFrames({"update", "name: cam\nset: {}\nunset: []"}, request),
              service_registry::RequestCode::MissingChange);
}

TEST(ServiceRegistryTest, UpdateTakesKeysToSetAndToUnset)
{
    service_registry::Request request{};
    ASSERT_EQ(ParseFrames({"update", "name: cam\nset: {MaxRestarts: 3}\nunset: Cwd\nrestart: true"}, request),
              service_registry::RequestCode::Ok);
    EXPECT_EQ(request.unset, (std::vector<std::string>{"Cwd"}));
    EXPECT_TRUE(request.restart);
    ASSERT_EQ(ParseFrames({"update", "name: cam\nunset: [Cwd, Display]"}, request), service_registry::RequestCode::Ok);
    EXPECT_EQ(request.unset, (std::vector<std::string>{"Cwd", "Display"}));
    EXPECT_EQ(ParseFrames({"update", "name: cam\nunset: [1, 2]"}, request), service_registry::RequestCode::WrongKind);
}

TEST(ServiceRegistryTest, OperationNamesAreTheWordsOfTheRequest)
{
    EXPECT_EQ(service_registry::OperationName(service_registry::Operation::Add), "add");
    EXPECT_EQ(service_registry::OperationName(service_registry::Operation::Remove), "remove");
    EXPECT_EQ(service_registry::OperationName(service_registry::Operation::List), "list");
}

TEST(ServiceRegistryTest, NameOfLettersDigitsAndPunctuationIsAccepted)
{
    EXPECT_EQ(service_registry::CheckName("cam-1.v2_X"), service_registry::NameCode::Ok);
    EXPECT_EQ(service_registry::CheckName(std::string(service_registry::max_name_length, 'a')),
              service_registry::NameCode::Ok);
}

TEST(ServiceRegistryTest, NameThatCannotBeAFileNameIsRefused)
{
    EXPECT_EQ(service_registry::CheckName(""), service_registry::NameCode::Empty);
    EXPECT_EQ(service_registry::CheckName(std::string(service_registry::max_name_length + 1, 'a')),
              service_registry::NameCode::TooLong);
    EXPECT_EQ(service_registry::CheckName(".hidden"), service_registry::NameCode::LeadingDot);
    EXPECT_EQ(service_registry::CheckName("a/b"), service_registry::NameCode::BadCharacter);
    EXPECT_EQ(service_registry::CheckName("a b"), service_registry::NameCode::BadCharacter);
    EXPECT_EQ(service_registry::CheckName("\xc3\xbc"), service_registry::NameCode::BadCharacter);
    EXPECT_FALSE(service_registry::DescribeName(service_registry::NameCode::BadCharacter, "a/b").empty());
    EXPECT_TRUE(service_registry::DescribeName(service_registry::NameCode::Ok, "cam").empty());
}

TEST(ServiceRegistryTest, NameDefinitionAddsTheName)
{
    config_section::ConfigValue definition = Parse("ExecutablePath: /bin/true\n");
    ASSERT_EQ(service_registry::NameDefinition(definition, "cam"), service_registry::EditCode::Ok);
    const config_section::ConfigValue* name = config_section::FindValue(definition, "ProcessName");
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->kind, config_section::ValueKind::String);
    EXPECT_EQ(name->text, "cam");
    EXPECT_EQ(definition.keys, (std::vector<std::string>{"ExecutablePath", "ProcessName"}));
}

TEST(ServiceRegistryTest, NameDefinitionAcceptsTheSameNameAndRefusesAnother)
{
    config_section::ConfigValue same = Parse("ProcessName: cam\nExecutablePath: /bin/true\n");
    EXPECT_EQ(service_registry::NameDefinition(same, "cam"), service_registry::EditCode::Ok);
    config_section::ConfigValue other = Parse("ProcessName: dog\nExecutablePath: /bin/true\n");
    EXPECT_EQ(service_registry::NameDefinition(other, "cam"), service_registry::EditCode::NameDiffers);
    config_section::ConfigValue list = Parse("[cam]");
    EXPECT_EQ(service_registry::NameDefinition(list, "cam"), service_registry::EditCode::NotMapping);
}

TEST(ServiceRegistryTest, ApplyUpdateSetsAndRemovesKeysAndKeepsThemSorted)
{
    config_section::ConfigValue definition = Parse("ProcessName: cam\nExecutablePath: /bin/true\nCwd: /tmp\n");
    const config_section::ConfigValue set = Parse("MaxRestarts: 3\nAlpha: x\nExecutablePath: /bin/false\n");
    ASSERT_EQ(service_registry::ApplyUpdate(definition, set, {"Cwd", "NotThere"}), service_registry::EditCode::Ok);
    EXPECT_EQ(definition.keys, (std::vector<std::string>{"Alpha", "ExecutablePath", "MaxRestarts", "ProcessName"}));
    EXPECT_EQ(config_section::FindValue(definition, "ExecutablePath")->text, "/bin/false");
    EXPECT_EQ(config_section::FindValue(definition, "Cwd"), nullptr);
}

TEST(ServiceRegistryTest, ApplyUpdateSetsAKeyItAlsoUnsets)
{
    config_section::ConfigValue definition = Parse("ProcessName: cam\nCwd: /tmp\n");
    ASSERT_EQ(service_registry::ApplyUpdate(definition, Parse("Cwd: /opt\n"), {"Cwd"}), service_registry::EditCode::Ok);
    EXPECT_EQ(config_section::FindValue(definition, "Cwd")->text, "/opt");
}

TEST(ServiceRegistryTest, ApplyUpdateRefusesTheNameAndLeavesTheDefinition)
{
    config_section::ConfigValue definition = Parse("ProcessName: cam\nCwd: /tmp\n");
    const config_section::ConfigValue before = definition;
    EXPECT_EQ(service_registry::ApplyUpdate(definition, Parse("ProcessName: dog\n"), {}),
              service_registry::EditCode::NameNotEditable);
    EXPECT_EQ(service_registry::ApplyUpdate(definition, config_section::ConfigValue{}, {"ProcessName"}),
              service_registry::EditCode::NameNotEditable);
    EXPECT_TRUE(Same(definition, before));
}

TEST(ServiceRegistryTest, EmittedYamlReadsBackAsTheSameDefinition)
{
    const config_section::ConfigValue definition = Parse(rich_definition);
    const std::string yaml = service_registry::EmitYaml(definition);
    EXPECT_TRUE(Same(Parse(yaml), definition)) << yaml;
}

TEST(ServiceRegistryTest, EmittedYamlPutsTheNameAndExecutableFirst)
{
    const std::string yaml = service_registry::EmitYaml(Parse(rich_definition));
    EXPECT_EQ(yaml.rfind("ProcessName: cam\nExecutablePath: /opt/cam/bin/server\n", 0), 0U) << yaml;
}

TEST(ServiceRegistryTest, EmittedYamlQuotesOnlyWhatNeedsIt)
{
    const std::string yaml = service_registry::EmitYaml(Parse(rich_definition));
    EXPECT_NE(yaml.find("  PYTHONPATH: /usr/lib\n"), std::string::npos) << yaml;
    EXPECT_NE(yaml.find("  - \"8080\"\n"), std::string::npos) << yaml;
    EXPECT_NE(yaml.find("  - \"true\"\n"), std::string::npos) << yaml;
    EXPECT_NE(yaml.find("BackoffMultiplier: 2.0\n"), std::string::npos) << yaml;
    EXPECT_NE(yaml.find("DependsOn: []\n"), std::string::npos) << yaml;
}

TEST(ServiceRegistryTest, ControlCharactersSurviveTheRoundTrip)
{
    config_section::ConfigValue definition = Parse("ProcessName: cam\n");
    config_section::ConfigValue odd{};
    odd.kind = config_section::ValueKind::Sequence;
    odd.children.push_back(Text(std::string{"bell\x07"} + "del\x7f" + "esc\x1b"));
    odd.children.push_back(Text("carriage\rreturn"));
    definition.keys.insert(definition.keys.begin(), "Odd"); // the reader gives the keys sorted
    definition.children.insert(definition.children.begin(), odd);
    const std::string yaml = service_registry::EmitYaml(definition);
    EXPECT_TRUE(Same(Parse(yaml), definition)) << yaml;
}

TEST(ServiceRegistryTest, IntegersInFloatKeysStayFloats)
{
    config_section::ConfigValue definition = Parse("ProcessName: cam\nBackoffMultiplier: 2.0\nHuge: 1e+20\n");
    const config_section::ConfigValue* huge = config_section::FindValue(definition, "Huge");
    ASSERT_NE(huge, nullptr);
    const std::string yaml = service_registry::EmitYaml(definition);
    EXPECT_TRUE(Same(Parse(yaml), definition)) << yaml;
}

TEST(ServiceRegistryTest, EmitJsonWritesEveryKind)
{
    const config_section::ConfigValue value =
        Parse("ProcessName: cam\nArgs: [\"a\\\"b\", 3]\nBackoffMultiplier: 2.0\nCheckHealth: true\nCwd: null\n"
              "EnvironmentVariables: {A: \"1\"}\n");
    EXPECT_EQ(service_registry::EmitJson(value),
              "{\"ProcessName\":\"cam\",\"Args\":[\"a\\\"b\",3],\"Cwd\":null,\"EnvironmentVariables\":{\"A\":\"1\"},"
              "\"BackoffMultiplier\":2.0,\"CheckHealth\":true}");
}

TEST(ServiceRegistryTest, JsonStringEscapesQuotesBackslashesAndControls)
{
    EXPECT_EQ(service_registry::JsonString("a\"b\\c\nd\x01"), "\"a\\\"b\\\\c\\u000ad\\u0001\"");
}

TEST(ServiceRegistryTest, JsonObjectWritesItsFieldsInOrder)
{
    service_registry::JsonObject object{};
    object.AddText("name", "cam");
    object.AddNumber("count", 3);
    object.AddFlag("ok", true);
    object.AddTexts("problems", {"one", "t\"wo"});
    object.AddJson("definition", "{}");
    EXPECT_EQ(object.Text(),
              "{\"name\":\"cam\",\"count\":3,\"ok\":true,\"problems\":[\"one\",\"t\\\"wo\"],\"definition\":{}}");
    EXPECT_EQ(service_registry::JsonObject{}.Text(), "{}");
}

TEST(ServiceRegistryTest, StatusTextIsTheWordOfTheFirstFrame)
{
    EXPECT_EQ(service_registry::StatusText(service_registry::ReplyStatus::Ok), "ok");
    EXPECT_EQ(service_registry::StatusText(service_registry::ReplyStatus::Refused), "refused");
    EXPECT_EQ(service_registry::StatusText(service_registry::ReplyStatus::Error), "error");
}

TEST(ServiceRegistryTest, NewProblemsAreThoseTheChangeBrings)
{
    EXPECT_EQ(service_registry::NewProblems({"a", "b", "b"}, {"b", "c", "b", "b"}),
              (std::vector<std::string>{"c", "b"}));
    EXPECT_TRUE(service_registry::NewProblems({"a"}, {}).empty());
}

TEST(ServiceRegistryTest, WriteMakesTheFolderAndLeavesNoTemporaryFile)
{
    const ScratchFolder scratch{};
    const service_registry::ServiceFiles files{scratch.Path() / "services.d"};
    ASSERT_EQ(files.Write("cam", "ProcessName: cam\n"), service_registry::StoreCode::Ok);
    EXPECT_EQ(Contents(scratch.Path() / "services.d" / "cam.yaml"), "ProcessName: cam\n");
    std::size_t entries = 0;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator{files.Folder()})
    {
        EXPECT_EQ(entry.path().filename(), "cam.yaml");
        ++entries;
    }

    EXPECT_EQ(entries, 1U);
}

TEST(ServiceRegistryTest, WriteReplacesAFile)
{
    const ScratchFolder scratch{};
    const service_registry::ServiceFiles files{scratch.Path()};
    ASSERT_EQ(files.Write("cam", "first\n"), service_registry::StoreCode::Ok);
    ASSERT_EQ(files.Write("cam", "second\n"), service_registry::StoreCode::Ok);
    std::string text;
    ASSERT_EQ(files.Read("cam", text), service_registry::StoreCode::Ok);
    EXPECT_EQ(text, "second\n");
}

TEST(ServiceRegistryTest, ExistsSeesFilesFoldersAndBrokenLinks)
{
    const ScratchFolder scratch{};
    const service_registry::ServiceFiles files{scratch.Path()};
    EXPECT_FALSE(files.Exists("cam"));
    ASSERT_EQ(files.Write("cam", "x\n"), service_registry::StoreCode::Ok);
    EXPECT_TRUE(files.Exists("cam"));
    std::filesystem::create_directory(scratch.Path() / "folder.yaml");
    EXPECT_TRUE(files.Exists("folder"));
    std::filesystem::create_symlink(scratch.Path() / "nowhere", scratch.Path() / "dangling.yaml");
    EXPECT_TRUE(files.Exists("dangling"));
    std::string text;
    EXPECT_EQ(files.Read("folder", text), service_registry::StoreCode::NotRead);
}

TEST(ServiceRegistryTest, RemoveDeletesAFileAndAMissingOneIsFine)
{
    const ScratchFolder scratch{};
    const service_registry::ServiceFiles files{scratch.Path()};
    ASSERT_EQ(files.Write("cam", "x\n"), service_registry::StoreCode::Ok);
    EXPECT_EQ(files.Remove("cam"), service_registry::StoreCode::Ok);
    EXPECT_FALSE(files.Exists("cam"));
    EXPECT_EQ(files.Remove("cam"), service_registry::StoreCode::Ok);
}

TEST(ServiceRegistryTest, RestoreUndoesAWriteOverAFile)
{
    const ScratchFolder scratch{};
    const service_registry::ServiceFiles files{scratch.Path()};
    ASSERT_EQ(files.Write("cam", "before\n"), service_registry::StoreCode::Ok);
    service_registry::Snapshot snapshot{};
    ASSERT_EQ(files.Save("cam", snapshot), service_registry::StoreCode::Ok);
    EXPECT_TRUE(snapshot.fileExisted);
    ASSERT_EQ(files.Write("cam", "after\n"), service_registry::StoreCode::Ok);
    ASSERT_EQ(files.Restore(snapshot), service_registry::StoreCode::Ok);
    EXPECT_EQ(Contents(files.FileOf("cam")), "before\n");
}

TEST(ServiceRegistryTest, RestoreUndoesARemove)
{
    const ScratchFolder scratch{};
    const service_registry::ServiceFiles files{scratch.Path()};
    ASSERT_EQ(files.Write("cam", "kept\n"), service_registry::StoreCode::Ok);
    service_registry::Snapshot snapshot{};
    ASSERT_EQ(files.Save("cam", snapshot), service_registry::StoreCode::Ok);
    ASSERT_EQ(files.Remove("cam"), service_registry::StoreCode::Ok);
    ASSERT_EQ(files.Restore(snapshot), service_registry::StoreCode::Ok);
    EXPECT_EQ(Contents(files.FileOf("cam")), "kept\n");
}

TEST(ServiceRegistryTest, RestoreRemovesANewFileAndTheFolderItMade)
{
    const ScratchFolder scratch{};
    const service_registry::ServiceFiles files{scratch.Path() / "services.d"};
    service_registry::Snapshot snapshot{};
    ASSERT_EQ(files.Save("cam", snapshot), service_registry::StoreCode::Ok);
    EXPECT_FALSE(snapshot.folderExisted);
    EXPECT_FALSE(snapshot.fileExisted);
    ASSERT_EQ(files.Write("cam", "new\n"), service_registry::StoreCode::Ok);
    ASSERT_EQ(files.Restore(snapshot), service_registry::StoreCode::Ok);
    EXPECT_FALSE(std::filesystem::exists(scratch.Path() / "services.d"));
}

TEST(ServiceRegistryTest, RestoreKeepsAFolderThatHoldsOtherFiles)
{
    const ScratchFolder scratch{};
    const service_registry::ServiceFiles files{scratch.Path() / "services.d"};
    service_registry::Snapshot snapshot{};
    ASSERT_EQ(files.Save("cam", snapshot), service_registry::StoreCode::Ok);
    ASSERT_EQ(files.Write("cam", "new\n"), service_registry::StoreCode::Ok);
    ASSERT_EQ(files.Write("other", "other\n"), service_registry::StoreCode::Ok);
    ASSERT_EQ(files.Restore(snapshot), service_registry::StoreCode::Ok);
    EXPECT_FALSE(files.Exists("cam"));
    EXPECT_TRUE(files.Exists("other"));
}
