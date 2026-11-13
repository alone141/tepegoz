#include <gtest/gtest.h>

#include "StatusClient.hpp"
#include "CommandWatch.hpp"
#include "ProcessManagerCommands.hpp"
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

process_manager::DetailedHealthReportStruct Record(const std::string& name, process_manager::RuntimeStateEnum state,
                                                   int pid)
{
    process_manager::DetailedHealthReportStruct record{};
    std::strncpy(record.processName, name.c_str(), sizeof(record.processName) - 1);
    record.pid = pid;
    record.runtimeHealth.state = state;
    record.memoryUsageInBytes = 64 * 1024 * 1024;
    record.snapshotTime = std::chrono::high_resolution_clock::now();
    record.runtimeHealth.startTime = record.snapshotTime - std::chrono::seconds{90};
    record.runtimeHealth.lastSeen = record.runtimeHealth.startTime;
    return record;
}

std::string Bytes(const std::vector<process_manager::DetailedHealthReportStruct>& records)
{
    std::string bytes(records.size() * sizeof(process_manager::DetailedHealthReportStruct), '\0');
    std::memcpy(bytes.data(), records.data(), bytes.size());
    return bytes;
}

// A stand-in manager: publishes its services' reports and applies the commands its broker gets, as the real one
// does, unless told to ignore them.
class FakeManager
{
public:
    explicit FakeManager(std::vector<process_manager::DetailedHealthReportStruct> services)
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

    status_client::Endpoints endpoints{};
    std::atomic<bool> ignoreCommands{false};

private:
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
                process_manager::ProcessManagerCommandStruct command{};
                std::memcpy(&command, frames.back().data(), std::min(frames.back().size(), sizeof(command)));
                const std::lock_guard<std::mutex> lock{mutex};
                received.push_back(frames.front().to_string() + " " + std::to_string(command.command) + " " +
                                   command.ServiceName);
                for (process_manager::DetailedHealthReportStruct& service : services)
                {
                    if (ignoreCommands || std::string_view{service.processName} != command.ServiceName)
                    {
                        continue;
                    }
                    if (command.command == process_manager::ProcessManagerCommandEnum::StopService)
                    {
                        service.pid = -1;
                        service.runtimeHealth.state = process_manager::RuntimeStateEnum::Stopped;
                    }
                    else
                    {
                        service.pid = nextPid++;
                        service.runtimeHealth.state = process_manager::RuntimeStateEnum::Running;
                    }
                }
            }

            const std::lock_guard<std::mutex> lock{mutex};
            for (process_manager::DetailedHealthReportStruct& service : services)
            {
                service.snapshotTime = std::chrono::high_resolution_clock::now();
                service.cpuUsageInUsec += 20000;
            }
            const std::string bytes = Bytes(services);
            (void)publisher.send(zmq::buffer(bytes), zmq::send_flags::dontwait);
        }
    }

    mutable std::mutex mutex;
    std::vector<process_manager::DetailedHealthReportStruct> services;
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
    const std::string bytes = Bytes({Record("a", process_manager::RuntimeStateEnum::Running, 10),
                                     Record("b", process_manager::RuntimeStateEnum::Unhealthy, 11)});
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
    const std::string bytes = Bytes({Record("a", process_manager::RuntimeStateEnum::Running, 10)});
    std::vector<status_table::ServiceSample> samples{};
    EXPECT_EQ(status_client::ParseReport(bytes.substr(1), samples), status_client::ParseCode::BadSize);
    EXPECT_TRUE(samples.empty());
}

TEST(StatusClientTest, PrintStatusShowsEveryService)
{
    FakeManager manager{{Record("control_loop", process_manager::RuntimeStateEnum::Running, 10),
                         Record("telemetry_bridge", process_manager::RuntimeStateEnum::Stopped, -1)}};
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
    FakeManager manager{{Record("a", process_manager::RuntimeStateEnum::Running, 10)}};
    testing::internal::CaptureStdout();
    const status_client::CommandCode code =
        status_client::SendCommand(command_watch::Command::Stop, "a", manager.endpoints, true, short_timeout);
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "a: Stopped\n");
    EXPECT_EQ(code, status_client::CommandCode::Done);
    EXPECT_EQ(manager.Received(), std::vector<std::string>{"PMC 79 a"});
}

TEST(StatusClientTest, RestartShowsTheNewProcess)
{
    FakeManager manager{{Record("a", process_manager::RuntimeStateEnum::Running, 10)}};
    testing::internal::CaptureStdout();
    const status_client::CommandCode code =
        status_client::SendCommand(command_watch::Command::Restart, "a", manager.endpoints, true, short_timeout);
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "a: Running, pid 500 (was 10)\n");
    EXPECT_EQ(code, status_client::CommandCode::Done);
}

TEST(StatusClientTest, UnknownServiceIsNotSent)
{
    FakeManager manager{{Record("viewer", process_manager::RuntimeStateEnum::Running, 10)}};
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
    FakeManager manager{{Record("a", process_manager::RuntimeStateEnum::Running, 10)}};
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
    FakeManager manager{{Record("a", process_manager::RuntimeStateEnum::Running, 10)}};
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
    FakeManager manager{{Record("a", process_manager::RuntimeStateEnum::Running, 10)}};
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
