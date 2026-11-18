#include <gtest/gtest.h>

#include "CommandWatch.hpp"
#include "StatusTable.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace
{

status_table::ServiceSample Sample(const std::string& name, status_table::ServiceState state, int pid)
{
    status_table::ServiceSample sample{};
    sample.name = name;
    sample.state = state;
    sample.pid = pid;
    return sample;
}

status_table::ServiceSample Running(const std::string& name, int pid)
{
    return Sample(name, status_table::ServiceState::Running, pid);
}

status_table::ServiceSample Starting(const std::string& name, int pid)
{
    return Sample(name, status_table::ServiceState::Starting, pid);
}

status_table::ServiceSample Stopped(const std::string& name)
{
    return Sample(name, status_table::ServiceState::Stopped, -1);
}

} // namespace

TEST(CommandWatchTest, FindsServiceByName)
{
    const std::optional<status_table::ServiceSample> found =
        command_watch::FindService({Running("a", 10), Running("b", 11)}, "b");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->pid, 11);
}

TEST(CommandWatchTest, UnknownNameIsNotFound)
{
    EXPECT_FALSE(command_watch::FindService({Running("viewer", 10)}, "viewr").has_value());
}

TEST(CommandWatchTest, StopIsDoneOnceStopped)
{
    command_watch::CommandWatch watch{command_watch::Command::Stop, "a", {Running("a", 10)}};
    std::vector<std::string> lines{};
    EXPECT_EQ(watch.Update({Running("a", 10)}, lines), command_watch::WatchCode::Waiting);
    EXPECT_TRUE(lines.empty());
    EXPECT_EQ(watch.Update({Stopped("a")}, lines), command_watch::WatchCode::Done);
    EXPECT_EQ(lines, std::vector<std::string>{"a: Stopped"});
}

TEST(CommandWatchTest, StopOfAStoppedServiceSaysSo)
{
    command_watch::CommandWatch watch{command_watch::Command::Stop, "a", {Stopped("a")}};
    std::vector<std::string> lines{};
    EXPECT_EQ(watch.Update({Stopped("a")}, lines), command_watch::WatchCode::Done);
    EXPECT_EQ(lines, std::vector<std::string>{"a: was not running"});
}

TEST(CommandWatchTest, StartShowsStartingThenRunning)
{
    command_watch::CommandWatch watch{command_watch::Command::Start, "a", {Stopped("a")}};
    std::vector<std::string> lines{};
    EXPECT_EQ(watch.Update({Starting("a", 7)}, lines), command_watch::WatchCode::Waiting);
    EXPECT_EQ(watch.Update({Starting("a", 7)}, lines), command_watch::WatchCode::Waiting);
    EXPECT_EQ(watch.Update({Running("a", 7)}, lines), command_watch::WatchCode::Done);
    EXPECT_EQ(lines, (std::vector<std::string>{"a: Starting, pid 7, waiting for its first heartbeat",
                                               "a: Running, pid 7"}));
}

TEST(CommandWatchTest, StartOfARunningServiceSaysSo)
{
    command_watch::CommandWatch watch{command_watch::Command::Start, "a", {Running("a", 10)}};
    std::vector<std::string> lines{};
    EXPECT_EQ(watch.Update({Running("a", 10)}, lines), command_watch::WatchCode::Done);
    EXPECT_EQ(lines, std::vector<std::string>{"a: Running, pid 10 (it was already running)"});
}

TEST(CommandWatchTest, StartThatExitsAgainFails)
{
    command_watch::CommandWatch watch{command_watch::Command::Start, "a", {Stopped("a")}};
    std::vector<std::string> lines{};
    EXPECT_EQ(watch.Update({Starting("a", 7)}, lines), command_watch::WatchCode::Waiting);
    EXPECT_EQ(watch.Update({Stopped("a")}, lines), command_watch::WatchCode::Failed);
    EXPECT_EQ(lines.back(), "a: started, then stopped again; its log says why");
}

TEST(CommandWatchTest, StartWithoutHeartbeatsFails)
{
    command_watch::CommandWatch watch{command_watch::Command::Start, "a", {Stopped("a")}};
    std::vector<std::string> lines{};
    EXPECT_EQ(watch.Update({Sample("a", status_table::ServiceState::Unhealthy, 7)}, lines),
              command_watch::WatchCode::Failed);
    EXPECT_EQ(lines, std::vector<std::string>{"a: Unhealthy, pid 7; it sends no heartbeats"});
}

TEST(CommandWatchTest, StoppedBeforeTheStartIsStillWaiting)
{
    command_watch::CommandWatch watch{command_watch::Command::Start, "a", {Stopped("a")}};
    std::vector<std::string> lines{};
    EXPECT_EQ(watch.Update({Stopped("a")}, lines), command_watch::WatchCode::Waiting);
    EXPECT_TRUE(lines.empty());
}

TEST(CommandWatchTest, RestartWaitsForANewProcess)
{
    command_watch::CommandWatch watch{command_watch::Command::Restart, "a", {Running("a", 5)}};
    std::vector<std::string> lines{};
    EXPECT_EQ(watch.Update({Running("a", 5)}, lines), command_watch::WatchCode::Waiting);
    EXPECT_EQ(watch.Update({Stopped("a")}, lines), command_watch::WatchCode::Waiting);
    EXPECT_EQ(watch.Update({Running("a", 6)}, lines), command_watch::WatchCode::Done);
    EXPECT_EQ(lines, std::vector<std::string>{"a: Running, pid 6 (was 5)"});
}

TEST(CommandWatchTest, RestartOfAStoppedServiceStartsIt)
{
    command_watch::CommandWatch watch{command_watch::Command::Restart, "a", {Stopped("a")}};
    std::vector<std::string> lines{};
    EXPECT_EQ(watch.Update({Running("a", 6)}, lines), command_watch::WatchCode::Done);
    EXPECT_EQ(lines, std::vector<std::string>{"a: Running, pid 6"});
}

TEST(CommandWatchTest, DisableIsDoneOnceTheServiceLeaves)
{
    command_watch::CommandWatch watch{command_watch::Command::Disable, "a", {Running("a", 5), Running("b", 6)}};
    std::vector<std::string> lines{};
    EXPECT_EQ(watch.Update({Running("a", 5), Running("b", 6)}, lines), command_watch::WatchCode::Waiting);
    EXPECT_EQ(watch.Update({Running("b", 6)}, lines), command_watch::WatchCode::Done);
    EXPECT_EQ(lines, std::vector<std::string>{"a: disabled; it comes back when the manager restarts"});
}

TEST(CommandWatchTest, StopAllWaitsForEveryService)
{
    command_watch::CommandWatch watch{command_watch::Command::Stop, command_watch::all_services,
                                      {Running("a", 5), Running("b", 6)}};
    std::vector<std::string> lines{};
    EXPECT_EQ(watch.Update({Stopped("a"), Running("b", 6)}, lines), command_watch::WatchCode::Waiting);
    EXPECT_EQ(watch.Update({Stopped("a"), Stopped("b")}, lines), command_watch::WatchCode::Done);
    EXPECT_EQ(lines, (std::vector<std::string>{"a: Stopped", "b: Stopped"}));
}

TEST(CommandWatchTest, StartAllWantsANewProcessForEveryService)
{
    command_watch::CommandWatch watch{command_watch::Command::Start, command_watch::all_services,
                                      {Running("a", 5), Stopped("b")}};
    std::vector<std::string> lines{};
    EXPECT_EQ(watch.Update({Running("a", 5), Stopped("b")}, lines), command_watch::WatchCode::Waiting);
    EXPECT_TRUE(lines.empty());
    // A disabled service comes back with the rebuild.
    EXPECT_EQ(watch.Update({Running("a", 8), Running("b", 9), Starting("c", 10)}, lines),
              command_watch::WatchCode::Waiting);
    EXPECT_EQ(watch.Update({Running("a", 8), Running("b", 9), Running("c", 10)}, lines),
              command_watch::WatchCode::Done);
    EXPECT_EQ(lines, (std::vector<std::string>{"a: Running, pid 8 (was 5)", "b: Running, pid 9",
                                               "c: Starting, pid 10, waiting for its first heartbeat",
                                               "c: Running, pid 10"}));
}

TEST(CommandWatchTest, AllIsFailedWhenOneFails)
{
    command_watch::CommandWatch watch{command_watch::Command::Restart, command_watch::all_services,
                                      {Running("a", 5), Running("b", 6)}};
    std::vector<std::string> lines{};
    EXPECT_EQ(watch.Update({Running("a", 8), Starting("b", 9)}, lines), command_watch::WatchCode::Waiting);
    EXPECT_EQ(watch.Update({Running("a", 8), Stopped("b")}, lines), command_watch::WatchCode::Failed);
}

TEST(CommandWatchTest, DescribesWhatIsStillOnItsWay)
{
    command_watch::CommandWatch start{command_watch::Command::Start, "a", {Stopped("a")}};
    command_watch::CommandWatch starting{command_watch::Command::Start, "b", {Stopped("b")}};
    command_watch::CommandWatch stop{command_watch::Command::Stop, "c", {Running("c", 5)}};
    std::vector<std::string> lines{};
    start.Update({Stopped("a")}, lines);
    starting.Update({Starting("b", 7)}, lines);
    stop.Update({Running("c", 5)}, lines);
    lines.clear();
    const std::chrono::seconds waited{10};
    start.DescribeWaiting(waited, lines);
    starting.DescribeWaiting(waited, lines);
    stop.DescribeWaiting(waited, lines);
    EXPECT_EQ(lines, (std::vector<std::string>{
                         "a: not started after 10s; it waits while a service ahead of it in the start order is in "
                         "its grace period or being stopped",
                         "b: still Starting after 10s, pid 7, no heartbeat yet",
                         "c: still Running after 10s, pid 5; a stop can take twice its ForceKillTimeoutMs"}));
}

TEST(CommandWatchTest, SettledServicesAreNotDescribed)
{
    command_watch::CommandWatch watch{command_watch::Command::Stop, "a", {Running("a", 5)}};
    std::vector<std::string> lines{};
    watch.Update({Stopped("a")}, lines);
    lines.clear();
    watch.DescribeWaiting(std::chrono::seconds{10}, lines);
    EXPECT_TRUE(lines.empty());
}
