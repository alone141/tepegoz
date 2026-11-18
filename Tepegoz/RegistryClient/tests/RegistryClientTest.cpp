#include <gtest/gtest.h>

#include "RegistryClient.hpp"
#include "ConfigSection.hpp"
#include "ServiceRegistry.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#include <zmq.hpp>
#include <zmq_addon.hpp>

namespace
{

const std::filesystem::path here{"/home/user/work"};

constexpr std::chrono::milliseconds short_timeout{1000};

// A folder of its own for one test, removed with the test.
class ScratchFolder
{
public:
    ScratchFolder()
    {
        char pattern[] = "/tmp/registry_client_test_XXXXXX";
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

    void Write(const std::string& name, const std::string& text) const
    {
        std::ofstream file{path / name, std::ios::binary};
        file << text;
    }

private:
    std::filesystem::path path;
};

// A stand-in service registry: a ROUTER that keeps every request and answers each with the same frames, or not at
// all when it has none.
class StandInRegistry
{
public:
    explicit StandInRegistry(std::vector<std::string> reply)
        : answer{std::move(reply)}
        , router{context, zmq::socket_type::router}
    {
        router.set(zmq::sockopt::linger, 0);
        router.bind("tcp://127.0.0.1:*");
        endpoint = router.get(zmq::sockopt::last_endpoint);
        thread = std::thread{[this] { Run(); }};
    }

    ~StandInRegistry()
    {
        running = false;
        thread.join();
    }

    // Neither copied nor moved: its thread holds this.
    StandInRegistry(const StandInRegistry&) = delete;
    StandInRegistry& operator=(const StandInRegistry&) = delete;
    StandInRegistry(StandInRegistry&&) = delete;
    StandInRegistry& operator=(StandInRegistry&&) = delete;

    std::vector<std::vector<std::string>> Received() const
    {
        const std::lock_guard<std::mutex> lock{mutex};
        return received;
    }

    std::string endpoint;

private:
    void Run()
    {
        while (running)
        {
            zmq::pollitem_t items[] = {{router.handle(), 0, ZMQ_POLLIN, 0}};
            zmq::poll(items, 1, std::chrono::milliseconds{50});
            if ((items[0].revents & ZMQ_POLLIN) == 0)
            {
                continue;
            }

            std::vector<zmq::message_t> frames;
            if (!zmq::recv_multipart(router, std::back_inserter(frames), zmq::recv_flags::dontwait).has_value())
            {
                continue;
            }

            std::vector<std::string> request;
            for (std::size_t i = 1; i < frames.size(); ++i)
            {
                request.push_back(frames[i].to_string());
            }

            {
                const std::lock_guard<std::mutex> lock{mutex};
                received.push_back(request);
            }

            if (answer.empty())
            {
                continue;
            }

            (void)router.send(frames[0], zmq::send_flags::sndmore);
            for (std::size_t i = 0; i < answer.size(); ++i)
            {
                const zmq::send_flags flags = i + 1 < answer.size() ? zmq::send_flags::sndmore : zmq::send_flags::none;
                (void)router.send(zmq::buffer(answer[i]), flags);
            }
        }
    }

    std::vector<std::string> answer;
    zmq::context_t context{1};
    zmq::socket_t router;
    mutable std::mutex mutex;
    std::vector<std::vector<std::string>> received;
    std::atomic<bool> running{true};
    std::thread thread;
};

registry_client::VerbLine Line(registry_client::Verb verb, std::vector<std::string> words)
{
    registry_client::VerbLine line{};
    line.verb = verb;
    line.words = std::move(words);
    return line;
}

// The request's frames, or the problem when it cannot be built.
std::vector<std::string> Frames(const registry_client::VerbLine& line, const std::filesystem::path& folder = here)
{
    std::vector<std::string> frames;
    std::string problem;
    if (registry_client::BuildRequest(line, folder, frames, problem) != registry_client::BuildCode::Ok)
    {
        return {"problem", problem};
    }

    return frames;
}

// The problem a command line that cannot be built has; empty when it can be built.
std::string Problem(const registry_client::VerbLine& line, registry_client::BuildCode expected)
{
    std::vector<std::string> frames;
    std::string problem;
    EXPECT_EQ(registry_client::BuildRequest(line, here, frames, problem), expected);
    EXPECT_TRUE(frames.empty());
    return problem;
}

config_section::ConfigValue Value(std::string_view text)
{
    config_section::ConfigValue value{};
    std::string problem;
    EXPECT_EQ(registry_client::ReadValue(text, value, problem), registry_client::ValueCode::Ok) << problem;
    return value;
}

config_section::ConfigValue Parse(const std::string& text)
{
    config_section::ConfigValue value{};
    std::string error;
    EXPECT_EQ(config_section::ParseConfigText(text, value, error), config_section::LoadCode::Ok) << error;
    return value;
}

bool Contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

registry_client::Answer Ok(const std::string& json)
{
    return registry_client::Answer{"ok", json};
}

const std::string list_reply =
    R"({"version":1,"operation":"list","message":"2 services","folder":"/etc/pm/services.d","services":[)"
    R"({"name":"anchor","origin":"main","file":"/etc/pm/main.yaml","enabled":true,"loaded":true,)"
    R"("state":"Running","pid":1234,"workMode":"Auto","restartToApply":false},)"
    R"({"name":"camera","origin":"registered","file":"/etc/pm/services.d/camera.yaml","enabled":true,"loaded":true,)"
    R"("state":"Running","pid":4321,"workMode":"Auto","restartToApply":true},)"
    R"({"name":"broken","origin":"registered","file":"/etc/pm/services.d/broken.yaml","enabled":false,)"
    R"("problem":"/etc/pm/services.d/broken.yaml: not valid YAML: line 1","loaded":false,"state":"NotLoaded",)"
    R"("pid":-1,"workMode":"","restartToApply":false}]})";

const std::string show_reply =
    R"({"version":1,"operation":"show","name":"camera","message":"'camera' is registered",)"
    R"("origin":"registered","file":"/etc/pm/services.d/camera.yaml","enabled":true,)"
    R"("definition":{"ProcessName":"camera","ExecutablePath":"/opt/cam/bin/server"},)"
    R"("yaml":"ProcessName: camera\nExecutablePath: /opt/cam/bin/server\n",)"
    R"("loaded":true,"state":"Running","pid":4321,"workMode":"Auto","restartToApply":false})";

const std::string add_reply =
    R"({"version":1,"operation":"add","name":"camera","outcome":"starting",)"
    R"("message":"'camera' is registered and starts","file":"/etc/pm/services.d/camera.yaml",)"
    R"("yaml":"ProcessName: camera\nExecutablePath: /opt/cam/bin/server\n",)"
    R"("reload":{"added":1,"removed":0,"changed":0,"kept":0,"skipped":0,"unchanged":1}})";

const std::string refused_reply =
    R"({"version":1,"operation":"add","name":"camera",)"
    R"("message":"the change would bring 2 problems; nothing was written",)"
    R"("problems":["service 'camera': ExecutablePath does not exists","service 'camera': unknown key 'Colour'"]})";

} // namespace

TEST(RegistryClientTest, FindVerbFindsTheVerbAsTheCommand)
{
    const std::vector<std::string> args{"add", "camera", "/opt/cam/bin/server"};
    EXPECT_EQ(registry_client::FindVerb(args), registry_client::Verb::Add);
}

TEST(RegistryClientTest, FindVerbIgnoresOtherCommands)
{
    const std::vector<std::string> args{"status", "--once"};
    EXPECT_FALSE(registry_client::FindVerb(args).has_value());
}

TEST(RegistryClientTest, FindVerbSkipsTheValueOfALongOption)
{
    const std::vector<std::string> args{"--registry", "target-01", "list"};
    EXPECT_EQ(registry_client::FindVerb(args), registry_client::Verb::List);
}

TEST(RegistryClientTest, FindVerbTakesTheWordAfterRestartForAServiceName)
{
    const std::vector<std::string> args{"--restart", "add"};
    EXPECT_FALSE(registry_client::FindVerb(args).has_value());
}

TEST(RegistryClientTest, FindVerbSkipsTheValueOfAShortOption)
{
    const std::vector<std::string> args{"-s", "list"};
    EXPECT_FALSE(registry_client::FindVerb(args).has_value());
}

TEST(RegistryClientTest, FindVerbReadsAValueJoinedToAShortOption)
{
    const std::vector<std::string> args{"-cmain.yaml", "list"};
    EXPECT_EQ(registry_client::FindVerb(args), registry_client::Verb::List);
}

TEST(RegistryClientTest, FindVerbReadsTheCommandAfterDoubleDash)
{
    const std::vector<std::string> args{"--", "show", "camera"};
    EXPECT_EQ(registry_client::FindVerb(args), registry_client::Verb::Show);
}

TEST(RegistryClientTest, VerbNamesReadBack)
{
    for (const registry_client::Verb verb : {registry_client::Verb::Add, registry_client::Verb::Set,
                                             registry_client::Verb::Replace, registry_client::Verb::Remove,
                                             registry_client::Verb::Show, registry_client::Verb::List})
    {
        EXPECT_EQ(registry_client::VerbOf(registry_client::VerbName(verb)), verb);
    }
}

TEST(RegistryClientTest, NumberValueIsANumber)
{
    const config_section::ConfigValue value = Value("3");
    EXPECT_EQ(value.kind, config_section::ValueKind::Integer);
    EXPECT_EQ(value.text, "3");
}

TEST(RegistryClientTest, FloatValueIsAFloat)
{
    EXPECT_EQ(Value("2.5").kind, config_section::ValueKind::Float);
}

TEST(RegistryClientTest, BooleanValueIsABoolean)
{
    const config_section::ConfigValue value = Value("false");
    EXPECT_EQ(value.kind, config_section::ValueKind::Boolean);
    EXPECT_EQ(value.text, "false");
}

TEST(RegistryClientTest, NullValueIsNull)
{
    EXPECT_EQ(Value("~").kind, config_section::ValueKind::Null);
}

TEST(RegistryClientTest, ListValueIsAList)
{
    const config_section::ConfigValue value = Value("[--port, 9090]");
    ASSERT_EQ(value.kind, config_section::ValueKind::Sequence);
    ASSERT_EQ(value.children.size(), 2U);
    EXPECT_EQ(value.children[0].text, "--port");
    EXPECT_EQ(value.children[1].kind, config_section::ValueKind::Integer);
}

TEST(RegistryClientTest, MappingValueIsAMapping)
{
    const config_section::ConfigValue value = Value("{MODE: fast}");
    ASSERT_EQ(value.kind, config_section::ValueKind::Mapping);
    EXPECT_EQ(config_section::FindValue(value, "MODE")->text, "fast");
}

TEST(RegistryClientTest, QuotedNumberIsAText)
{
    const config_section::ConfigValue value = Value("\"8080\"");
    EXPECT_EQ(value.kind, config_section::ValueKind::String);
    EXPECT_EQ(value.text, "8080");
}

TEST(RegistryClientTest, QuotedEmptyTextIsAnEmptyText)
{
    const config_section::ConfigValue value = Value("''");
    EXPECT_EQ(value.kind, config_section::ValueKind::String);
    EXPECT_EQ(value.text, "");
}

TEST(RegistryClientTest, ColonValueIsTheText)
{
    const config_section::ConfigValue value = Value(":1");
    EXPECT_EQ(value.kind, config_section::ValueKind::String);
    EXPECT_EQ(value.text, ":1");
}

TEST(RegistryClientTest, PathWithASpaceIsTheText)
{
    const config_section::ConfigValue value = Value("/opt/x y");
    EXPECT_EQ(value.kind, config_section::ValueKind::String);
    EXPECT_EQ(value.text, "/opt/x y");
}

TEST(RegistryClientTest, HashAndColonSpaceStayInTheText)
{
    const config_section::ConfigValue value = Value("a # b: c");
    EXPECT_EQ(value.kind, config_section::ValueKind::String);
    EXPECT_EQ(value.text, "a # b: c");
}

TEST(RegistryClientTest, WordThatIsNoNumberIsTheText)
{
    const config_section::ConfigValue value = Value("Pipe");
    EXPECT_EQ(value.kind, config_section::ValueKind::String);
    EXPECT_EQ(value.text, "Pipe");
}

TEST(RegistryClientTest, EmptyValueIsRefused)
{
    config_section::ConfigValue value{};
    std::string problem;
    EXPECT_EQ(registry_client::ReadValue("", value, problem), registry_client::ValueCode::Empty);
}

TEST(RegistryClientTest, UnclosedQuoteIsRefused)
{
    config_section::ConfigValue value{};
    std::string problem;
    EXPECT_EQ(registry_client::ReadValue("\"abc", value, problem), registry_client::ValueCode::BadQuotes);
    EXPECT_TRUE(Contains(problem, "starts with a quote")) << problem;
}

TEST(RegistryClientTest, TextAfterTheClosingQuoteIsRefused)
{
    config_section::ConfigValue value{};
    std::string problem;
    EXPECT_EQ(registry_client::ReadValue("\"a\" b", value, problem), registry_client::ValueCode::BadQuotes);
    EXPECT_TRUE(Contains(problem, "closing quote must be its last character")) << problem;
}

TEST(RegistryClientTest, DoubledSingleQuoteStaysInTheText)
{
    EXPECT_EQ(Value("'it''s'").text, "it's");
}

TEST(RegistryClientTest, EscapedDoubleQuoteStaysInTheText)
{
    EXPECT_EQ(Value("\"say \\\"hi\\\"\"").text, "say \"hi\"");
}

TEST(RegistryClientTest, UnclosedListIsRefused)
{
    config_section::ConfigValue value{};
    std::string problem;
    EXPECT_EQ(registry_client::ReadValue("[a, b", value, problem), registry_client::ValueCode::BadCollection);
    EXPECT_TRUE(Contains(problem, "does not end with ]")) << problem;
}

TEST(RegistryClientTest, ListWithTextAfterItIsRefused)
{
    config_section::ConfigValue value{};
    std::string problem;
    EXPECT_EQ(registry_client::ReadValue("[a, b]]", value, problem), registry_client::ValueCode::BadCollection);
    EXPECT_TRUE(Contains(problem, "starts with [ or {")) << problem;
}

TEST(RegistryClientTest, RelativeExecutableIsMadeAbsolute)
{
    EXPECT_EQ(registry_client::AbsoluteProgram("bin/server", here), "/home/user/work/bin/server");
}

TEST(RegistryClientTest, DotsOfARelativeExecutableAreResolved)
{
    EXPECT_EQ(registry_client::AbsoluteProgram("../app/./server", here), "/home/user/app/server");
}

TEST(RegistryClientTest, BareProgramNameIsLeftAlone)
{
    EXPECT_EQ(registry_client::AbsoluteProgram("sleep", here), "sleep");
}

TEST(RegistryClientTest, AbsoluteExecutableIsLeftAlone)
{
    EXPECT_EQ(registry_client::AbsoluteProgram("/opt/cam/../cam/server", here), "/opt/cam/../cam/server");
}

TEST(RegistryClientTest, NoRegistryIsTheDefaultEndpoint)
{
    EXPECT_EQ(registry_client::RegistryEndpoint(""), "tcp://localhost:6671");
}

TEST(RegistryClientTest, RegistryHostGetsTcpAndThePort)
{
    EXPECT_EQ(registry_client::RegistryEndpoint("target-01"), "tcp://target-01:6671");
}

TEST(RegistryClientTest, RegistryHostWithAPortGetsTcp)
{
    EXPECT_EQ(registry_client::RegistryEndpoint("10.0.0.5:7000"), "tcp://10.0.0.5:7000");
}

TEST(RegistryClientTest, RegistryIpv6AddressGetsThePort)
{
    EXPECT_EQ(registry_client::RegistryEndpoint("[::1]"), "tcp://[::1]:6671");
}

TEST(RegistryClientTest, RegistryEndpointIsKeptAsGiven)
{
    EXPECT_EQ(registry_client::RegistryEndpoint("tcp://target-01:6671"), "tcp://target-01:6671");
}

TEST(RegistryClientTest, AddSendsTheExecutableAndItsArgs)
{
    const registry_client::VerbLine line =
        Line(registry_client::Verb::Add, {"camera", "/opt/cam/bin/server", "--port", "8080"});
    const std::vector<std::string> expected{
        "add",
        R"({"name":"camera","definition":{"ExecutablePath":"/opt/cam/bin/server","Args":["--port","8080"]}})"};
    EXPECT_EQ(Frames(line), expected);
}

TEST(RegistryClientTest, AddWithoutArgsSendsNoArgs)
{
    const registry_client::VerbLine line = Line(registry_client::Verb::Add, {"camera", "/opt/cam/bin/server"});
    const std::vector<std::string> expected{
        "add", R"({"name":"camera","definition":{"ExecutablePath":"/opt/cam/bin/server"}})"};
    EXPECT_EQ(Frames(line), expected);
}

TEST(RegistryClientTest, AddMakesARelativeExecutableAbsolute)
{
    const registry_client::VerbLine line = Line(registry_client::Verb::Add, {"camera", "./bin/server"});
    EXPECT_TRUE(Contains(Frames(line).back(), R"("ExecutablePath":"/home/user/work/bin/server")"));
}

TEST(RegistryClientTest, AddLeavesABareProgramNameToTheManager)
{
    const registry_client::VerbLine line = Line(registry_client::Verb::Add, {"sleeper", "sleep", "60"});
    EXPECT_TRUE(Contains(Frames(line).back(), R"("ExecutablePath":"sleep","Args":["60"])"));
}

TEST(RegistryClientTest, AddWithSetAddsKeysOfTheirKind)
{
    registry_client::VerbLine line = Line(registry_client::Verb::Add, {"camera", "/opt/cam/bin/server"});
    line.sets = {"MaxRestarts=3", "CheckHealth=false", "Display=:1"};
    const std::vector<std::string> expected{
        "add", R"({"name":"camera","definition":{"ExecutablePath":"/opt/cam/bin/server","MaxRestarts":3,)"
               R"("CheckHealth":false,"Display":":1"}})"};
    EXPECT_EQ(Frames(line), expected);
}

TEST(RegistryClientTest, AddTakesArgsFromSetWhenNoWordsFollowTheExecutable)
{
    registry_client::VerbLine line = Line(registry_client::Verb::Add, {"camera", "/opt/cam/bin/server"});
    line.sets = {"Args=[--port, 9090]"};
    EXPECT_TRUE(Contains(Frames(line).back(), R"("Args":["--port",9090])"));
}

TEST(RegistryClientTest, AddWithNoStartSendsStartFalse)
{
    registry_client::VerbLine line = Line(registry_client::Verb::Add, {"camera", "/opt/cam/bin/server"});
    line.noStart = true;
    EXPECT_TRUE(Contains(Frames(line).back(), R"(,"start":false})"));
}

TEST(RegistryClientTest, AddWithDryRunSendsDryRun)
{
    registry_client::VerbLine line = Line(registry_client::Verb::Add, {"camera", "/opt/cam/bin/server"});
    line.dryRun = true;
    EXPECT_TRUE(Contains(Frames(line).back(), R"(,"dryRun":true})"));
}

TEST(RegistryClientTest, AddFromFileSendsItsKeys)
{
    const ScratchFolder folder{};
    folder.Write("camera.yaml", "ExecutablePath: /opt/cam/bin/server\nArgs: [--port, \"8080\"]\nMaxRestarts: 3\n");
    registry_client::VerbLine line = Line(registry_client::Verb::Add, {"camera"});
    line.from = "camera.yaml";
    const std::vector<std::string> expected{
        "add", R"({"name":"camera","definition":{"ExecutablePath":"/opt/cam/bin/server","Args":["--port","8080"],)"
               R"("MaxRestarts":3}})"};
    EXPECT_EQ(Frames(line, folder.Path()), expected);
}

TEST(RegistryClientTest, SetOverridesAKeyOfTheFile)
{
    const ScratchFolder folder{};
    folder.Write("camera.yaml", "ExecutablePath: /opt/cam/bin/server\nMaxRestarts: 3\n");
    registry_client::VerbLine line = Line(registry_client::Verb::Add, {"camera"});
    line.from = "camera.yaml";
    line.sets = {"MaxRestarts=7"};
    EXPECT_TRUE(Contains(Frames(line, folder.Path()).back(), R"("MaxRestarts":7})"));
}

TEST(RegistryClientTest, AddFromFileWithAnExecutableIsRefused)
{
    registry_client::VerbLine line = Line(registry_client::Verb::Add, {"camera", "/opt/cam/bin/server"});
    line.from = "camera.yaml";
    EXPECT_TRUE(Contains(Problem(line, registry_client::BuildCode::BadWords), "takes no EXECUTABLE"));
}

TEST(RegistryClientTest, AddWithoutAnExecutableIsRefused)
{
    const registry_client::VerbLine line = Line(registry_client::Verb::Add, {"camera"});
    EXPECT_TRUE(Contains(Problem(line, registry_client::BuildCode::BadWords), "add NAME EXECUTABLE [ARG...]"));
}

TEST(RegistryClientTest, AddWithTheExecutableAlsoInSetIsRefused)
{
    registry_client::VerbLine line = Line(registry_client::Verb::Add, {"camera", "/opt/cam/bin/server"});
    line.sets = {"ExecutablePath=/opt/other"};
    EXPECT_TRUE(Contains(Problem(line, registry_client::BuildCode::BadWords), "ExecutablePath is given twice"));
}

TEST(RegistryClientTest, AddWithArgsInWordsAndInSetIsRefused)
{
    registry_client::VerbLine line = Line(registry_client::Verb::Add, {"camera", "/opt/cam/bin/server", "-v"});
    line.sets = {"Args=[--port, 9090]"};
    EXPECT_TRUE(Contains(Problem(line, registry_client::BuildCode::BadWords), "Args is given twice"));
}

TEST(RegistryClientTest, SetSendsUpdateWithTheKeys)
{
    const registry_client::VerbLine line =
        Line(registry_client::Verb::Set, {"camera", "MaxRestarts=3", "Cwd=/opt/x y", "Display=a=b"});
    const std::vector<std::string> expected{
        "update", R"({"name":"camera","set":{"Cwd":"/opt/x y","MaxRestarts":3,"Display":"a=b"}})"};
    EXPECT_EQ(Frames(line), expected);
}

TEST(RegistryClientTest, SetWithUnsetSendsTheKeysToRemove)
{
    registry_client::VerbLine line = Line(registry_client::Verb::Set, {"camera"});
    line.unsets = {"Cwd", "DependsOn"};
    const std::vector<std::string> expected{"update", R"({"name":"camera","unset":["Cwd","DependsOn"]})"};
    EXPECT_EQ(Frames(line), expected);
}

TEST(RegistryClientTest, SetWithRestartAndDryRunSendsBoth)
{
    registry_client::VerbLine line = Line(registry_client::Verb::Set, {"camera", "MaxRestarts=3"});
    line.restart = true;
    line.dryRun = true;
    EXPECT_TRUE(Contains(Frames(line).back(), R"(,"restart":true,"dryRun":true})"));
}

TEST(RegistryClientTest, SetTakesSetOptionsLikeWords)
{
    registry_client::VerbLine line = Line(registry_client::Verb::Set, {"camera"});
    line.sets = {"MaxRestarts=3"};
    EXPECT_TRUE(Contains(Frames(line).back(), R"("set":{"MaxRestarts":3})"));
}

TEST(RegistryClientTest, SetWithoutChangesIsRefused)
{
    const registry_client::VerbLine line = Line(registry_client::Verb::Set, {"camera"});
    EXPECT_TRUE(Contains(Problem(line, registry_client::BuildCode::BadWords), "set needs keys to change"));
}

TEST(RegistryClientTest, SetWordWithoutEqualsIsRefused)
{
    const registry_client::VerbLine line = Line(registry_client::Verb::Set, {"camera", "MaxRestarts"});
    EXPECT_TRUE(Contains(Problem(line, registry_client::BuildCode::BadWords), "'MaxRestarts' is not Key=Value"));
}

TEST(RegistryClientTest, SetWordWithoutKeyIsRefused)
{
    const registry_client::VerbLine line = Line(registry_client::Verb::Set, {"camera", "=3"});
    EXPECT_TRUE(Contains(Problem(line, registry_client::BuildCode::BadWords), "has no key"));
}

TEST(RegistryClientTest, SetOfAKeyTwiceIsRefused)
{
    const registry_client::VerbLine line = Line(registry_client::Verb::Set, {"camera", "MaxRestarts=3", "MaxRestarts=4"});
    EXPECT_TRUE(Contains(Problem(line, registry_client::BuildCode::BadWords), "MaxRestarts is given twice"));
}

TEST(RegistryClientTest, SetAndUnsetOfOneKeyIsRefused)
{
    registry_client::VerbLine line = Line(registry_client::Verb::Set, {"camera", "Cwd=/opt"});
    line.unsets = {"Cwd"};
    EXPECT_TRUE(Contains(Problem(line, registry_client::BuildCode::BadWords), "Cwd is both set and unset"));
}

TEST(RegistryClientTest, SetWithAnEmptyValueIsRefusedWithTheWaysOut)
{
    const registry_client::VerbLine line = Line(registry_client::Verb::Set, {"camera", "Cwd="});
    const std::string problem = Problem(line, registry_client::BuildCode::BadValue);
    EXPECT_TRUE(Contains(problem, "'Cwd=' has no value")) << problem;
    EXPECT_TRUE(Contains(problem, "'Cwd=\"\"'")) << problem;
    EXPECT_TRUE(Contains(problem, "--unset Cwd")) << problem;
}

TEST(RegistryClientTest, SetWithAnUnreadableValueNamesTheKey)
{
    const registry_client::VerbLine line = Line(registry_client::Verb::Set, {"camera", "Args=[--port"});
    EXPECT_TRUE(Contains(Problem(line, registry_client::BuildCode::BadValue), "the value of Args cannot be read"));
}

TEST(RegistryClientTest, ReplaceSendsTheFileAsTheDefinition)
{
    const ScratchFolder folder{};
    folder.Write("camera.yaml", "ProcessName: camera\nExecutablePath: /opt/cam/bin/server\n");
    registry_client::VerbLine line = Line(registry_client::Verb::Replace, {"camera"});
    line.from = (folder.Path() / "camera.yaml").string();
    line.restart = true;
    const std::vector<std::string> expected{
        "replace", R"({"name":"camera","definition":{"ProcessName":"camera","ExecutablePath":"/opt/cam/bin/server"},)"
                   R"("restart":true})"};
    EXPECT_EQ(Frames(line), expected);
}

TEST(RegistryClientTest, ReplaceWithoutAFileIsRefused)
{
    const registry_client::VerbLine line = Line(registry_client::Verb::Replace, {"camera"});
    EXPECT_TRUE(Contains(Problem(line, registry_client::BuildCode::BadWords), "replace NAME --from FILE"));
}

TEST(RegistryClientTest, ReplaceFromAMissingFileIsRefused)
{
    registry_client::VerbLine line = Line(registry_client::Verb::Replace, {"camera"});
    line.from = "nowhere.yaml";
    EXPECT_TRUE(Contains(Problem(line, registry_client::BuildCode::BadFile), "/home/user/work/nowhere.yaml"));
}

TEST(RegistryClientTest, ReplaceFromAFileThatIsNotYamlIsRefused)
{
    const ScratchFolder folder{};
    folder.Write("camera.yaml", "ProcessName: 'camera\n");
    registry_client::VerbLine line = Line(registry_client::Verb::Replace, {"camera"});
    line.from = (folder.Path() / "camera.yaml").string();
    EXPECT_TRUE(Contains(Problem(line, registry_client::BuildCode::BadFile), "is not valid YAML"));
}

TEST(RegistryClientTest, ReplaceFromAFileWithoutAMappingIsRefused)
{
    const ScratchFolder folder{};
    folder.Write("camera.yaml", "- camera\n");
    registry_client::VerbLine line = Line(registry_client::Verb::Replace, {"camera"});
    line.from = (folder.Path() / "camera.yaml").string();
    EXPECT_TRUE(Contains(Problem(line, registry_client::BuildCode::BadFile), "does not hold the keys of one service"));
}

TEST(RegistryClientTest, RemoveSendsForceAndDryRun)
{
    registry_client::VerbLine line = Line(registry_client::Verb::Remove, {"camera"});
    line.force = true;
    line.dryRun = true;
    const std::vector<std::string> expected{"remove", R"({"name":"camera","force":true,"dryRun":true})"};
    EXPECT_EQ(Frames(line), expected);
}

TEST(RegistryClientTest, RemoveOfTwoNamesIsRefused)
{
    const registry_client::VerbLine line = Line(registry_client::Verb::Remove, {"camera", "viewer"});
    EXPECT_TRUE(Contains(Problem(line, registry_client::BuildCode::BadWords), "remove takes one NAME"));
}

TEST(RegistryClientTest, ShowSendsTheName)
{
    const std::vector<std::string> expected{"show", R"({"name":"camera"})"};
    EXPECT_EQ(Frames(Line(registry_client::Verb::Show, {"camera"})), expected);
}

TEST(RegistryClientTest, ShowWithoutANameIsRefused)
{
    const registry_client::VerbLine line = Line(registry_client::Verb::Show, {});
    EXPECT_TRUE(Contains(Problem(line, registry_client::BuildCode::BadWords), "show needs the name of a service"));
}

TEST(RegistryClientTest, ListSendsOneFrame)
{
    EXPECT_EQ(Frames(Line(registry_client::Verb::List, {})), std::vector<std::string>{"list"});
}

TEST(RegistryClientTest, ListWithWordsIsRefused)
{
    const registry_client::VerbLine line = Line(registry_client::Verb::List, {"camera"});
    EXPECT_TRUE(Contains(Problem(line, registry_client::BuildCode::BadWords), "list takes no words"));
}

TEST(RegistryClientTest, OptionOfAnotherVerbIsRefused)
{
    registry_client::VerbLine line = Line(registry_client::Verb::Remove, {"camera"});
    line.restart = true;
    EXPECT_EQ(Problem(line, registry_client::BuildCode::BadOption), "remove does not take --restart");
}

TEST(RegistryClientTest, ChangePrintsTheMessageAndTheFile)
{
    const registry_client::Printed printed =
        registry_client::RenderReply(Line(registry_client::Verb::Add, {"camera", "/opt/cam/bin/server"}), Ok(add_reply));
    EXPECT_EQ(printed.out, "'camera' is registered and starts\nFile: /etc/pm/services.d/camera.yaml\n");
    EXPECT_EQ(printed.err, "");
    EXPECT_EQ(printed.outcome, registry_client::Outcome::Done);
}

TEST(RegistryClientTest, DryRunPrintsOnlyTheYamlOnStandardOutput)
{
    registry_client::VerbLine line = Line(registry_client::Verb::Add, {"camera", "/opt/cam/bin/server"});
    line.dryRun = true;
    const std::string reply =
        R"({"version":1,"operation":"add","name":"camera","outcome":"valid",)"
        R"("message":"the change is valid; nothing was written","file":"/etc/pm/services.d/camera.yaml",)"
        R"("yaml":"ProcessName: camera\nExecutablePath: /opt/cam/bin/server\n"})";
    const registry_client::Printed printed = registry_client::RenderReply(line, Ok(reply));
    EXPECT_EQ(printed.out, "ProcessName: camera\nExecutablePath: /opt/cam/bin/server\n");
    EXPECT_EQ(printed.err, "The change is valid; nothing was written\nFile: /etc/pm/services.d/camera.yaml\n");
    EXPECT_EQ(printed.outcome, registry_client::Outcome::Done);
}

TEST(RegistryClientTest, RemoveDryRunPrintsThatTheRemovalIsPossible)
{
    registry_client::VerbLine line = Line(registry_client::Verb::Remove, {"camera"});
    line.dryRun = true;
    const std::string reply =
        R"({"version":1,"operation":"remove","name":"camera","outcome":"valid",)"
        R"("message":"'camera' can be removed; nothing was written","file":"/etc/pm/services.d/camera.yaml"})";
    const registry_client::Printed printed = registry_client::RenderReply(line, Ok(reply));
    EXPECT_EQ(printed.out, "'camera' can be removed; nothing was written\nFile: /etc/pm/services.d/camera.yaml\n");
    EXPECT_EQ(printed.err, "");
}

TEST(RegistryClientTest, RefusalPrintsTheMessageAndEachProblem)
{
    const registry_client::Printed printed = registry_client::RenderReply(
        Line(registry_client::Verb::Add, {"camera", "/opt/cam/bin/server"}), {"refused", refused_reply});
    EXPECT_EQ(printed.out, "");
    EXPECT_EQ(printed.err, "Refused: the change would bring 2 problems; nothing was written\n"
                           "  service 'camera': ExecutablePath does not exists\n"
                           "  service 'camera': unknown key 'Colour'\n");
    EXPECT_EQ(printed.outcome, registry_client::Outcome::Refused);
}

TEST(RegistryClientTest, ErrorReplyIsNotUnderstood)
{
    const registry_client::Printed printed = registry_client::RenderReply(
        Line(registry_client::Verb::List, {}), {"error", R"({"version":1,"message":"'lsit' is not an operation"})"});
    EXPECT_EQ(printed.err, "The manager did not understand the request: 'lsit' is not an operation\n");
    EXPECT_EQ(printed.outcome, registry_client::Outcome::NotUnderstood);
}

TEST(RegistryClientTest, UnknownStatusIsABadAnswer)
{
    const registry_client::Printed printed =
        registry_client::RenderReply(Line(registry_client::Verb::List, {}), {"maybe", "{}"});
    EXPECT_TRUE(Contains(printed.err, "its status is 'maybe'")) << printed.err;
    EXPECT_EQ(printed.outcome, registry_client::Outcome::BadAnswer);
}

TEST(RegistryClientTest, AnswerThatIsNotAnObjectIsABadAnswer)
{
    const registry_client::Printed printed =
        registry_client::RenderReply(Line(registry_client::Verb::List, {}), Ok("[1, 2"));
    EXPECT_TRUE(Contains(printed.err, "could not be read")) << printed.err;
    EXPECT_EQ(printed.outcome, registry_client::Outcome::BadAnswer);
}

TEST(RegistryClientTest, ListPrintsATableOfEveryService)
{
    const registry_client::Printed printed =
        registry_client::RenderReply(Line(registry_client::Verb::List, {}), Ok(list_reply));
    EXPECT_EQ(printed.out,
              "NAME    ORIGIN      STATE     PID  MARK  FILE\n"
              "anchor  main        Running  1234        /etc/pm/main.yaml\n"
              "camera  registered  Running  4321  *     /etc/pm/services.d/camera.yaml\n"
              "broken  registered  Skipped     -  !     /etc/pm/services.d/broken.yaml\n"
              "* restart to apply: a reloaded definition takes effect at the service's next start\n"
              "! /etc/pm/services.d/broken.yaml: not valid YAML: line 1\n");
    EXPECT_EQ(printed.err, "");
    EXPECT_EQ(printed.outcome, registry_client::Outcome::Done);
}

TEST(RegistryClientTest, ListCallsAServiceTheManagerDoesNotHaveDisabled)
{
    const std::string reply =
        R"({"version":1,"operation":"list","message":"1 services","folder":"/etc/pm/services.d","services":[)"
        R"({"name":"spare","origin":"main","file":"/etc/pm/main.yaml","enabled":false,"loaded":false,)"
        R"("state":"NotLoaded","pid":-1,"workMode":"","restartToApply":false}]})";
    const registry_client::Printed printed = registry_client::RenderReply(Line(registry_client::Verb::List, {}), Ok(reply));
    EXPECT_TRUE(Contains(printed.out, "spare  main    Disabled    -        /etc/pm/main.yaml\n")) << printed.out;
}

TEST(RegistryClientTest, JsonPrintsTheAnswerAsItCame)
{
    registry_client::VerbLine line = Line(registry_client::Verb::List, {});
    line.json = true;
    const registry_client::Printed printed = registry_client::RenderReply(line, Ok(list_reply));
    EXPECT_EQ(printed.out, list_reply + "\n");
    EXPECT_EQ(printed.err, "");
}

TEST(RegistryClientTest, JsonOfARefusalExitsAsARefusal)
{
    registry_client::VerbLine line = Line(registry_client::Verb::Show, {"nobody"});
    line.json = true;
    const std::string reply = R"({"version":1,"operation":"show","name":"nobody",)"
                              R"("message":"there is no service named 'nobody'","problems":[]})";
    const registry_client::Printed printed = registry_client::RenderReply(line, {"refused", reply});
    EXPECT_EQ(printed.out, reply + "\n");
    EXPECT_EQ(printed.outcome, registry_client::Outcome::Refused);
}

TEST(RegistryClientTest, ShowPrintsOnlyTheYamlOnStandardOutput)
{
    const registry_client::Printed printed =
        registry_client::RenderReply(Line(registry_client::Verb::Show, {"camera"}), Ok(show_reply));
    EXPECT_EQ(printed.out, "ProcessName: camera\nExecutablePath: /opt/cam/bin/server\n");
    EXPECT_EQ(printed.err, "'camera' is registered\nFile: /etc/pm/services.d/camera.yaml\n"
                           "State: Running, pid 4321, work mode Auto\n");
}

TEST(RegistryClientTest, ShowOfASkippedFilePrintsItsText)
{
    const std::string reply =
        R"({"version":1,"operation":"show","name":"broken","message":"/etc/pm/services.d/broken.yaml: not valid YAML",)"
        R"("origin":"registered","file":"/etc/pm/services.d/broken.yaml","definition":null,)"
        R"("problem":"/etc/pm/services.d/broken.yaml: not valid YAML","fileText":"ProcessName: [broken\n",)"
        R"("loaded":false,"state":"NotLoaded","pid":-1,"workMode":"","restartToApply":false})";
    const registry_client::Printed printed =
        registry_client::RenderReply(Line(registry_client::Verb::Show, {"broken"}), Ok(reply));
    EXPECT_EQ(printed.out, "ProcessName: [broken\n");
    EXPECT_EQ(printed.err, "/etc/pm/services.d/broken.yaml: not valid YAML\n"
                           "The file is skipped. To repair it: show broken > FILE, edit FILE, then replace broken "
                           "--from FILE\n"
                           "File: /etc/pm/services.d/broken.yaml\n"
                           "State: Skipped\n");
}

TEST(RegistryClientTest, ShowOutputReadsBackThroughReplaceFrom)
{
    const config_section::ConfigValue definition = Parse(
        "ProcessName: camera\nExecutablePath: /opt/cam/bin/server\nArgs: [--port, \"8080\"]\nCwd: \"/opt/x y\"\n"
        "Display: \":1\"\nEnvironmentVariables:\n  MODE: \"a # b: c\"\nMaxRestarts: 3\nBackoffMultiplier: 2.0\n"
        "CheckHealth: false\n");
    service_registry::JsonObject reply{};
    reply.AddText("message", "'camera' is registered");
    reply.AddText("file", "/etc/pm/services.d/camera.yaml");
    reply.AddJson("definition", service_registry::EmitJson(definition));
    reply.AddText("yaml", service_registry::EmitYaml(definition));
    const registry_client::Printed printed =
        registry_client::RenderReply(Line(registry_client::Verb::Show, {"camera"}), Ok(reply.Text()));

    const ScratchFolder folder{};
    folder.Write("camera.yaml", printed.out);
    registry_client::VerbLine line = Line(registry_client::Verb::Replace, {"camera"});
    line.from = "camera.yaml";
    const std::vector<std::string> frames = Frames(line, folder.Path());
    ASSERT_EQ(frames.size(), 2U);
    const config_section::ConfigValue sent = Parse(frames[1]);
    const config_section::ConfigValue* replaced = config_section::FindValue(sent, "definition");
    ASSERT_NE(replaced, nullptr);
    EXPECT_EQ(service_registry::EmitJson(*replaced), service_registry::EmitJson(definition));
}

TEST(RegistryClientTest, ExitCodesFollowTheOutcome)
{
    EXPECT_EQ(registry_client::ExitCode(registry_client::Outcome::Done), 0);
    EXPECT_EQ(registry_client::ExitCode(registry_client::Outcome::Refused), 1);
    EXPECT_EQ(registry_client::ExitCode(registry_client::Outcome::NotUnderstood), 1);
    EXPECT_EQ(registry_client::ExitCode(registry_client::Outcome::BadAnswer), 1);
    EXPECT_EQ(registry_client::ExitCode(registry_client::Outcome::BadCommandLine), 1);
    EXPECT_EQ(registry_client::ExitCode(registry_client::Outcome::NotReachable), 1);
    EXPECT_EQ(registry_client::ExitCode(registry_client::Outcome::TimedOut), 2);
}

TEST(RegistryClientTest, SendRequestDeliversTheFramesAndReturnsTheAnswer)
{
    const StandInRegistry registry{std::vector<std::string>{"ok", add_reply}};
    const std::vector<std::string> frames{"show", R"({"name":"camera"})"};
    registry_client::Answer answer{};
    EXPECT_EQ(registry_client::SendRequest(registry.endpoint, frames, short_timeout, answer),
              registry_client::SendCode::Answered);
    EXPECT_EQ(answer.status, "ok");
    EXPECT_EQ(answer.json, add_reply);
    EXPECT_EQ(registry.Received(), std::vector<std::vector<std::string>>{frames});
}

TEST(RegistryClientTest, AnswerOfOneFrameIsABadAnswer)
{
    const StandInRegistry registry{std::vector<std::string>{"ok"}};
    registry_client::Answer answer{};
    EXPECT_EQ(registry_client::SendRequest(registry.endpoint, {"list"}, short_timeout, answer),
              registry_client::SendCode::BadAnswer);
}

TEST(RegistryClientTest, RunVerbPrintsTheAnswer)
{
    const StandInRegistry registry{std::vector<std::string>{"ok", list_reply}};
    registry_client::VerbLine line = Line(registry_client::Verb::List, {});
    line.registry = registry.endpoint;
    testing::internal::CaptureStdout();
    const registry_client::Outcome outcome = registry_client::RunVerb(line, here, short_timeout);
    const std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(outcome, registry_client::Outcome::Done);
    EXPECT_TRUE(Contains(out, "camera  registered  Running  4321  *")) << out;
    EXPECT_EQ(registry.Received(), std::vector<std::vector<std::string>>{{"list"}});
}

TEST(RegistryClientTest, RunVerbOfARefusalExitsWithOne)
{
    const StandInRegistry registry{std::vector<std::string>{"refused", refused_reply}};
    registry_client::VerbLine line = Line(registry_client::Verb::Add, {"camera", "/opt/cam/bin/server"});
    line.registry = registry.endpoint;
    testing::internal::CaptureStderr();
    const registry_client::Outcome outcome = registry_client::RunVerb(line, here, short_timeout);
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(registry_client::ExitCode(outcome), 1);
    EXPECT_TRUE(Contains(err, "  service 'camera': unknown key 'Colour'\n")) << err;
}

TEST(RegistryClientTest, RunVerbWithBadWordsSendsNothing)
{
    const StandInRegistry registry{std::vector<std::string>{"ok", add_reply}};
    registry_client::VerbLine line = Line(registry_client::Verb::Add, {"camera"});
    line.registry = registry.endpoint;
    testing::internal::CaptureStderr();
    const registry_client::Outcome outcome = registry_client::RunVerb(line, here, short_timeout);
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(outcome, registry_client::Outcome::BadCommandLine);
    EXPECT_TRUE(Contains(err, "Use --help")) << err;
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    EXPECT_TRUE(registry.Received().empty());
}

TEST(RegistryClientTest, NoAnswerTimesOut)
{
    const StandInRegistry registry{std::vector<std::string>{}};
    registry_client::VerbLine line = Line(registry_client::Verb::List, {});
    line.registry = registry.endpoint;
    testing::internal::CaptureStderr();
    const registry_client::Outcome outcome = registry_client::RunVerb(line, here, short_timeout);
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(outcome, registry_client::Outcome::TimedOut);
    EXPECT_EQ(registry_client::ExitCode(outcome), 2);
    EXPECT_TRUE(Contains(err, "No answer from the process manager within 1s")) << err;
}

TEST(RegistryClientTest, UnreachableRegistryIsReported)
{
    registry_client::VerbLine line = Line(registry_client::Verb::List, {});
    line.registry = "tcp://127.0.0.1:1";
    testing::internal::CaptureStderr();
    const registry_client::Outcome outcome = registry_client::RunVerb(line, here, short_timeout);
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(outcome, registry_client::Outcome::NotReachable);
    EXPECT_EQ(registry_client::ExitCode(outcome), 1);
    EXPECT_TRUE(Contains(err, "not reachable at tcp://127.0.0.1:1")) << err;
}

TEST(RegistryClientTest, EndpointThatCannotBeUsedIsReported)
{
    registry_client::VerbLine line = Line(registry_client::Verb::List, {});
    line.registry = "nowhere://target-01";
    testing::internal::CaptureStderr();
    const registry_client::Outcome outcome = registry_client::RunVerb(line, here, short_timeout);
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(outcome, registry_client::Outcome::BadCommandLine);
    EXPECT_TRUE(Contains(err, "'nowhere://target-01' is not an endpoint")) << err;
}
