#include <gtest/gtest.h>

#include "StatusClient.hpp"
#include "CommandWatch.hpp"
#include "TepegozCommands.hpp"
#include "RuntimeHealth.hpp"
#include "StatusTable.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <zmq.hpp>
#include <zmq_addon.hpp>

namespace
{

constexpr std::chrono::milliseconds report_period{100};
constexpr std::chrono::seconds short_timeout{2};

tepegoz::ReloadReport Answer(tepegoz::ReloadResult result)
{
    tepegoz::ReloadReport answer{};
    answer.command = tepegoz::CommandCode::ReloadConfig;
    answer.result = result;
    return answer;
}

std::string AnswerBytes(const tepegoz::ReloadReport& answer)
{
    std::string bytes(sizeof(answer), '\0');
    std::memcpy(bytes.data(), &answer, sizeof(answer));
    return bytes;
}

status_table::ServiceSample Sample(const std::string& name, bool marked)
{
    status_table::ServiceSample sample{};
    sample.name = name;
    sample.restartToApply = marked;
    return sample;
}

tepegoz::DetailedHealthReport Record(const std::string& name, tepegoz::RuntimeState state,
                                                   int pid)
{
    tepegoz::DetailedHealthReport record{};
    std::strncpy(record.processName, name.c_str(), sizeof(record.processName) - 1);
    record.pid = pid;
    record.runtimeHealth.state = state;
    record.memoryUsageInBytes = 64 * 1024 * 1024;
    record.snapshotTime = std::chrono::high_resolution_clock::now();
    record.runtimeHealth.startTime = record.snapshotTime - std::chrono::seconds{90};
    record.runtimeHealth.lastSeen = record.runtimeHealth.startTime;
    return record;
}

std::string Bytes(const std::vector<tepegoz::DetailedHealthReport>& records)
{
    std::string bytes(records.size() * sizeof(tepegoz::DetailedHealthReport), '\0');
    std::memcpy(bytes.data(), records.data(), bytes.size());
    return bytes;
}

// A stand-in manager: publishes its services' reports and applies the commands its broker gets, as the real one
// does, unless told to ignore them.
class FakeManager
{
public:
    explicit FakeManager(std::vector<tepegoz::DetailedHealthReport> services)
        : services{std::move(services)}
        , publisher{context, zmq::socket_type::pub}
        , broker{context, zmq::socket_type::router}
    {
        publisher.set(zmq::sockopt::linger, 0);
        broker.set(zmq::sockopt::linger, 0);
        publisher.bind("tcp://127.0.0.1:*");
        broker.bind("tcp://127.0.0.1:*");
        endpoints.reports = publisher.get(zmq::sockopt::last_endpoint);
        endpoints.commands = broker.get(zmq::sockopt::last_endpoint);
        thread = std::thread{[this] { Run(); }};
    }

    FakeManager(const FakeManager&) = delete;
    FakeManager& operator=(const FakeManager&) = delete;

    ~FakeManager()
    {
        running = false;
        thread.join();
    }

    std::vector<std::string> Received() const
    {
        const std::lock_guard<std::mutex> lock{mutex};
        return received;
    }

    // What a reload answers, and the services it leaves.
    void OnReload(const tepegoz::ReloadReport& answer,
                  std::vector<tepegoz::DetailedHealthReport> servicesAfter)
    {
        const std::lock_guard<std::mutex> lock{mutex};
        reloadAnswer = answer;
        reloaded = std::move(servicesAfter);
    }

    status_client::Endpoints endpoints{};
    std::atomic<bool> ignoreCommands{false};

private:
    // Called with the mutex held.
    void Reload(const std::string& sender, const std::string& args)
    {
        received.back() += args;
        if (ignoreCommands)
        {
            return;
        }

        services = reloaded;
        if (args == tepegoz::reload_without_reply)
        {
            return;
        }

        (void)broker.send(zmq::buffer(sender), zmq::send_flags::sndmore);
        (void)broker.send(zmq::buffer(tepegoz::comm::tepegoz_id), zmq::send_flags::sndmore);
        (void)broker.send(zmq::buffer(&reloadAnswer, sizeof(reloadAnswer)), zmq::send_flags::none);
    }

    void Run()
    {
        int nextPid = 500;
        while (running)
        {
            zmq::pollitem_t items[] = {{broker.handle(), 0, ZMQ_POLLIN, 0}};
            zmq::poll(items, 1, report_period);
            if ((items[0].revents & ZMQ_POLLIN) != 0)
            {
                std::vector<zmq::message_t> frames{};
                (void)zmq::recv_multipart(broker, std::back_inserter(frames));
                tepegoz::CommandMessage command{};
                std::memcpy(&command, frames.back().data(), std::min(frames.back().size(), sizeof(command)));
                const std::lock_guard<std::mutex> lock{mutex};
                received.push_back(frames.front().to_string() + " " + std::to_string(static_cast<int>(command.command)) + " " +
                                   command.serviceName);
                if (command.command == tepegoz::CommandCode::ReloadConfig)
                {
                    Reload(frames.front().to_string(), std::string{command.args});
                }
                for (tepegoz::DetailedHealthReport& service : services)
                {
                    if (ignoreCommands || std::string_view{service.processName} != command.serviceName)
                    {
                        continue;
                    }
                    if (command.command == tepegoz::CommandCode::StopService)
                    {
                        service.pid = -1;
                        service.runtimeHealth.state = tepegoz::RuntimeState::Stopped;
                    }
                    else
                    {
                        service.pid = nextPid++;
                        service.runtimeHealth.state = tepegoz::RuntimeState::Running;
                    }
                }
            }

            const std::lock_guard<std::mutex> lock{mutex};
            for (tepegoz::DetailedHealthReport& service : services)
            {
                service.snapshotTime = std::chrono::high_resolution_clock::now();
                service.cpuUsageInUsec += 20000;
            }
            const std::string bytes = Bytes(services);
            (void)publisher.send(zmq::buffer(bytes), zmq::send_flags::dontwait);
        }
    }

    mutable std::mutex mutex;
    std::vector<tepegoz::DetailedHealthReport> services;
    tepegoz::ReloadReport reloadAnswer{};
    std::vector<tepegoz::DetailedHealthReport> reloaded;
    std::vector<std::string> received;
    zmq::context_t context{1};
    zmq::socket_t publisher;
    zmq::socket_t broker;
    std::atomic<bool> running{true};
    std::thread thread;
};

} // namespace

TEST(StatusClientTest, ParsesEveryRecord)
{
    const std::string bytes = Bytes({Record("a", tepegoz::RuntimeState::Running, 10),
                                     Record("b", tepegoz::RuntimeState::Unhealthy, 11)});
    std::vector<status_table::ServiceSample> samples{};
    ASSERT_EQ(status_client::ParseReport(bytes, samples), status_client::ParseCode::Ok);
    ASSERT_EQ(samples.size(), 2u);
    EXPECT_EQ(samples[0].name, "a");
    EXPECT_EQ(samples[1].state, status_table::ServiceState::Unhealthy);
    EXPECT_EQ(samples[1].pid, 11);
    EXPECT_EQ(samples[0].snapshotNs - samples[0].startNs, 90LL * 1000000000LL);
}

TEST(StatusClientTest, PartialRecordIsBadSize)
{
    const std::string bytes = Bytes({Record("a", tepegoz::RuntimeState::Running, 10)});
    std::vector<status_table::ServiceSample> samples{};
    EXPECT_EQ(status_client::ParseReport(bytes.substr(1), samples), status_client::ParseCode::BadSize);
    EXPECT_TRUE(samples.empty());
}

TEST(StatusClientTest, PrintStatusShowsEveryService)
{
    FakeManager manager{{Record("control_loop", tepegoz::RuntimeState::Running, 10),
                         Record("telemetry_bridge", tepegoz::RuntimeState::Stopped, -1)}};
    testing::internal::CaptureStdout();
    const status_client::StatusCode code = status_client::PrintStatus(manager.endpoints);
    const std::string out = testing::internal::GetCapturedStdout();
    ASSERT_EQ(code, status_client::StatusCode::Ok);
    EXPECT_NE(out.find("2 services: 1 running, 1 stopped"), std::string::npos);
    EXPECT_NE(out.find("telemetry_bridge  Stopped"), std::string::npos);

    // 20 ms of CPU every 100 ms: about 20 % of a core, in the fifth column.
    const std::size_t row = out.find("\ncontrol_loop ");
    ASSERT_NE(row, std::string::npos);
    std::istringstream fields{out.substr(row + 1, out.find('\n', row + 1) - row - 1)};
    std::vector<std::string> cells{};
    std::string cell{};
    while (fields >> cell)
    {
        cells.push_back(cell);
    }
    ASSERT_GE(cells.size(), 5u);
    const double cpu = std::stod(cells[4]);
    EXPECT_GT(cpu, 15.0);
    EXPECT_LT(cpu, 25.0);
}

TEST(StatusClientTest, PrintStatusWithoutManagerSaysSo)
{
    status_client::Endpoints nowhere{};
    nowhere.reports = "tcp://127.0.0.1:1";
    testing::internal::CaptureStderr();
    EXPECT_EQ(status_client::PrintStatus(nowhere), status_client::StatusCode::NoReport);
    EXPECT_NE(testing::internal::GetCapturedStderr().find("is it running?"), std::string::npos);
}

TEST(StatusClientTest, StopIsFollowedUntilStopped)
{
    FakeManager manager{{Record("a", tepegoz::RuntimeState::Running, 10)}};
    testing::internal::CaptureStdout();
    const status_client::CommandCode code =
        status_client::SendCommand(command_watch::Command::Stop, "a", manager.endpoints, true, short_timeout);
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "a: Stopped\n");
    EXPECT_EQ(code, status_client::CommandCode::Done);
    EXPECT_EQ(manager.Received(), std::vector<std::string>{"PMC 79 a"});
}

TEST(StatusClientTest, RestartShowsTheNewProcess)
{
    FakeManager manager{{Record("a", tepegoz::RuntimeState::Running, 10)}};
    testing::internal::CaptureStdout();
    const status_client::CommandCode code =
        status_client::SendCommand(command_watch::Command::Restart, "a", manager.endpoints, true, short_timeout);
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "a: Running, pid 500 (was 10)\n");
    EXPECT_EQ(code, status_client::CommandCode::Done);
}

TEST(StatusClientTest, UnknownServiceIsNotSent)
{
    FakeManager manager{{Record("viewer", tepegoz::RuntimeState::Running, 10)}};
    testing::internal::CaptureStderr();
    const status_client::CommandCode code =
        status_client::SendCommand(command_watch::Command::Stop, "viewr", manager.endpoints, true, short_timeout);
    EXPECT_NE(testing::internal::GetCapturedStderr().find("No service named 'viewr'. The manager has: viewer"),
              std::string::npos);
    EXPECT_EQ(code, status_client::CommandCode::NoSuchService);
    EXPECT_TRUE(manager.Received().empty());
}

TEST(StatusClientTest, IgnoredCommandTimesOut)
{
    FakeManager manager{{Record("a", tepegoz::RuntimeState::Running, 10)}};
    manager.ignoreCommands = true;
    testing::internal::CaptureStdout();
    const status_client::CommandCode code =
        status_client::SendCommand(command_watch::Command::Stop, "a", manager.endpoints, true, std::chrono::seconds{1});
    EXPECT_EQ(testing::internal::GetCapturedStdout(),
              "a: still Running after 1s, pid 10; a stop can take twice its ForceKillTimeoutMs\n");
    EXPECT_EQ(code, status_client::CommandCode::TimedOut);
    EXPECT_EQ(status_client::ExitCode(code), 2);
}

TEST(StatusClientTest, WithoutFollowingNothingIsPrinted)
{
    FakeManager manager{{Record("a", tepegoz::RuntimeState::Running, 10)}};
    testing::internal::CaptureStdout();
    const status_client::CommandCode code =
        status_client::SendCommand(command_watch::Command::Stop, "a", manager.endpoints, false, short_timeout);
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "");
    EXPECT_EQ(code, status_client::CommandCode::Done);
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + short_timeout;
    while (manager.Received().empty() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(report_period);
    }
    EXPECT_EQ(manager.Received(), std::vector<std::string>{"PMC 79 a"});
}

TEST(StatusClientTest, UnreachableBrokerIsReported)
{
    FakeManager manager{{Record("a", tepegoz::RuntimeState::Running, 10)}};
    status_client::Endpoints endpoints = manager.endpoints;
    endpoints.commands = "tcp://127.0.0.1:1";
    testing::internal::CaptureStderr();
    const status_client::CommandCode code =
        status_client::SendCommand(command_watch::Command::Stop, "a", endpoints, true, short_timeout);
    EXPECT_NE(testing::internal::GetCapturedStderr().find("not reachable at tcp://127.0.0.1:1"), std::string::npos);
    EXPECT_EQ(code, status_client::CommandCode::NotReachable);
}

TEST(StatusClientTest, LongNameIsRefused)
{
    testing::internal::CaptureStderr();
    const status_client::CommandCode code = status_client::SendCommand(
        command_watch::Command::Stop, std::string(32, 'x'), status_client::Endpoints{}, true, short_timeout);
    EXPECT_NE(testing::internal::GetCapturedStderr().find("longer than 31 bytes"), std::string::npos);
    EXPECT_EQ(code, status_client::CommandCode::BadTarget);
}

TEST(StatusClientTest, DisableAllIsRefused)
{
    testing::internal::CaptureStderr();
    const status_client::CommandCode code =
        status_client::SendCommand(command_watch::Command::Disable, command_watch::all_services,
                                   status_client::Endpoints{}, true, short_timeout);
    EXPECT_NE(testing::internal::GetCapturedStderr().find("not all"), std::string::npos);
    EXPECT_EQ(code, status_client::CommandCode::BadTarget);
}

TEST(StatusClientTest, ExitCodesFollowTheOutcome)
{
    EXPECT_EQ(status_client::ExitCode(status_client::CommandCode::Done), 0);
    EXPECT_EQ(status_client::ExitCode(status_client::CommandCode::Failed), 1);
    EXPECT_EQ(status_client::ExitCode(status_client::CommandCode::NoSuchService), 1);
    EXPECT_EQ(status_client::ExitCode(status_client::CommandCode::TimedOut), 2);
}

TEST(StatusClientTest, ParseReportCarriesTheRestartMark)
{
    tepegoz::DetailedHealthReport marked = Record("a", tepegoz::RuntimeState::Running, 10);
    marked.runtimeHealth.restartToApply = 1;
    const std::string bytes = Bytes({marked, Record("b", tepegoz::RuntimeState::Running, 11)});
    std::vector<status_table::ServiceSample> samples{};
    ASSERT_EQ(status_client::ParseReport(bytes, samples), status_client::ParseCode::Ok);
    ASSERT_EQ(samples.size(), 2u);
    EXPECT_TRUE(samples[0].restartToApply);
    EXPECT_FALSE(samples[1].restartToApply);
}

TEST(StatusClientTest, RestartMarkIsTheByteAfterTheState)
{
    tepegoz::DetailedHealthReport record{};
    record.runtimeHealth.restartToApply = 1;
    unsigned char bytes[sizeof(record)]{};
    std::memcpy(bytes, &record, sizeof(record));
    EXPECT_EQ(bytes[89], 1);
}

TEST(StatusClientTest, ParseReloadReplyReadsTheCounts)
{
    tepegoz::ReloadReport answer = Answer(tepegoz::ReloadResult::Applied);
    answer.added = 2;
    answer.removed = 1;
    answer.changed = 3;
    answer.kept = 4;
    answer.skipped = 5;
    answer.unchanged = 6;
    status_client::ReloadReply reply{};
    ASSERT_EQ(status_client::ParseReloadReply(AnswerBytes(answer), reply), status_client::ParseCode::Ok);
    EXPECT_TRUE(reply.applied);
    EXPECT_TRUE(reply.refusal.empty());
    EXPECT_EQ(reply.added, 2);
    EXPECT_EQ(reply.removed, 1);
    EXPECT_EQ(reply.changed, 3);
    EXPECT_EQ(reply.kept, 4);
    EXPECT_EQ(reply.skipped, 5);
    EXPECT_EQ(reply.unchanged, 6);
}

TEST(StatusClientTest, ParseReloadReplyWordsTheRefusal)
{
    status_client::ReloadReply reply{};
    ASSERT_EQ(status_client::ParseReloadReply(AnswerBytes(Answer(tepegoz::ReloadResult::FolderNotReadable)),
                                              reply),
              status_client::ParseCode::Ok);
    EXPECT_FALSE(reply.applied);
    EXPECT_EQ(reply.refusal, "the services.d folder could not be listed");
}

TEST(StatusClientTest, ParseReloadReplyRefusesAnotherSize)
{
    status_client::ReloadReply reply{};
    const std::string bytes = AnswerBytes(Answer(tepegoz::ReloadResult::Applied));
    EXPECT_EQ(status_client::ParseReloadReply(bytes.substr(1), reply), status_client::ParseCode::BadSize);
}

TEST(StatusClientTest, ParseReloadReplyRefusesAnotherCommand)
{
    tepegoz::ReloadReport answer = Answer(tepegoz::ReloadResult::Applied);
    answer.command = tepegoz::CommandCode::GetVersionInfo;
    status_client::ReloadReply reply{};
    EXPECT_EQ(status_client::ParseReloadReply(AnswerBytes(answer), reply), status_client::ParseCode::WrongCommand);
}

TEST(StatusClientTest, DescribeReloadNamesWhatTheReportsShow)
{
    status_client::ReloadReply reply{};
    reply.applied = true;
    reply.added = 1;
    reply.removed = 1;
    reply.changed = 1;
    reply.unchanged = 1;
    const std::vector<std::string> lines = status_client::DescribeReload(
        reply, {Sample("kept", false), Sample("gone", false), Sample("edited", false)},
        {Sample("kept", false), Sample("edited", true), Sample("new", false)}, true);
    EXPECT_EQ(lines, (std::vector<std::string>{
                         "Reload applied: 1 added, 1 removed, 1 changed, 0 kept, 0 not added, 1 unchanged",
                         "  added: new", "  removed: gone", "  restart to apply their new definition: edited"}));
}

TEST(StatusClientTest, DescribeReloadWithoutReportsGivesTheCounts)
{
    status_client::ReloadReply reply{};
    reply.applied = true;
    reply.kept = 1;
    reply.skipped = 2;
    const std::vector<std::string> lines = status_client::DescribeReload(reply, {}, {Sample("x", false)}, false);
    EXPECT_EQ(lines, (std::vector<std::string>{
                         "Reload applied: 0 added, 0 removed, 0 changed, 1 kept, 2 not added, 0 unchanged",
                         "  kept as they were, their new definition could not be read: 1 (the manager's log names them)",
                         "  not added, their definition could not be read: 2 (the manager's log names them)"}));
}

TEST(StatusClientTest, DescribeRefusedReloadSaysNothingChanged)
{
    status_client::ReloadReply reply{};
    reply.refusal = "the configuration file is not valid YAML";
    EXPECT_EQ(status_client::DescribeReload(reply, {}, {}, true),
              (std::vector<std::string>{"Reload refused: the configuration file is not valid YAML. Nothing was changed; "
                                        "the manager's log says more."}));
}

TEST(StatusClientTest, AppliedReloadIsToldWithTheNames)
{
    FakeManager manager{{Record("kept", tepegoz::RuntimeState::Running, 10),
                         Record("gone", tepegoz::RuntimeState::Running, 11)}};
    tepegoz::ReloadReport answer = Answer(tepegoz::ReloadResult::Applied);
    answer.added = 1;
    answer.removed = 1;
    answer.unchanged = 1;
    manager.OnReload(answer, {Record("kept", tepegoz::RuntimeState::Running, 10),
                              Record("new", tepegoz::RuntimeState::Stopped, -1)});
    testing::internal::CaptureStdout();
    const status_client::ReloadCode code = status_client::SendReload(manager.endpoints, true, short_timeout);
    EXPECT_EQ(testing::internal::GetCapturedStdout(),
              "Reload applied: 1 added, 1 removed, 0 changed, 0 kept, 0 not added, 1 unchanged\n"
              "  added: new\n"
              "  removed: gone\n");
    EXPECT_EQ(code, status_client::ReloadCode::Applied);
    EXPECT_EQ(status_client::ExitCode(code), 0);
    EXPECT_EQ(manager.Received(), std::vector<std::string>{"PMC 83 "});
}

TEST(StatusClientTest, RefusedReloadExitsWithOne)
{
    FakeManager manager{{Record("a", tepegoz::RuntimeState::Running, 10)}};
    manager.OnReload(Answer(tepegoz::ReloadResult::MainFileSyntaxError),
                     {Record("a", tepegoz::RuntimeState::Running, 10)});
    testing::internal::CaptureStderr();
    const status_client::ReloadCode code = status_client::SendReload(manager.endpoints, true, short_timeout);
    EXPECT_NE(testing::internal::GetCapturedStderr().find("Reload refused: the configuration file is not valid YAML"),
              std::string::npos);
    EXPECT_EQ(code, status_client::ReloadCode::Refused);
    EXPECT_EQ(status_client::ExitCode(code), 1);
}

TEST(StatusClientTest, UnansweredReloadTimesOut)
{
    FakeManager manager{{Record("a", tepegoz::RuntimeState::Running, 10)}};
    manager.ignoreCommands = true;
    testing::internal::CaptureStderr();
    const status_client::ReloadCode code = status_client::SendReload(manager.endpoints, true, std::chrono::seconds{1});
    EXPECT_NE(testing::internal::GetCapturedStderr().find("No answer from the process manager within 1s"),
              std::string::npos);
    EXPECT_EQ(code, status_client::ReloadCode::TimedOut);
    EXPECT_EQ(status_client::ExitCode(code), 2);
}

TEST(StatusClientTest, ReloadWithoutWaitingAsksForNoAnswer)
{
    FakeManager manager{{Record("a", tepegoz::RuntimeState::Running, 10)}};
    manager.OnReload(Answer(tepegoz::ReloadResult::Applied), {Record("a", tepegoz::RuntimeState::Running, 10)});
    testing::internal::CaptureStdout();
    const status_client::ReloadCode code = status_client::SendReload(manager.endpoints, false, short_timeout);
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "");
    EXPECT_EQ(code, status_client::ReloadCode::Sent);
    EXPECT_EQ(status_client::ExitCode(code), 0);
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + short_timeout;
    while (manager.Received().empty() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(report_period);
    }
    EXPECT_EQ(manager.Received(), std::vector<std::string>{"PMC 83 no-reply"});
}

TEST(StatusClientTest, ReloadToAnUnreachableBrokerIsReported)
{
    FakeManager manager{{Record("a", tepegoz::RuntimeState::Running, 10)}};
    status_client::Endpoints endpoints = manager.endpoints;
    endpoints.commands = "tcp://127.0.0.1:1";
    testing::internal::CaptureStderr();
    const status_client::ReloadCode code = status_client::SendReload(endpoints, true, short_timeout);
    EXPECT_NE(testing::internal::GetCapturedStderr().find("not reachable at tcp://127.0.0.1:1"), std::string::npos);
    EXPECT_EQ(code, status_client::ReloadCode::NotReachable);
    EXPECT_EQ(status_client::ExitCode(code), 1);
}
