#include <gtest/gtest.h>

#include "Process.hpp"

#include "ConfigSection.hpp"
#include "Logger.hpp"
#include "ProcessGroupBuilder.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <signal.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{

using Clock = std::chrono::high_resolution_clock;

constexpr std::chrono::milliseconds poll_period{5};
constexpr std::chrono::milliseconds wait_limit{3000};
constexpr std::chrono::milliseconds stop_limit{5000};

std::string Program(std::string_view name)
{
    return std::string{PMT_BIN_DIR} + "/" + std::string{name};
}

// A service entry as the manager's configuration holds it. Output goes to /dev/null unless the settings say otherwise.
std::string Service(std::string_view name, std::string_view program, std::string_view settings)
{
    std::string yaml = "ProcessName: " + std::string{name} + "\nExecutablePath: " + Program(program) + "\n" +
                       std::string{settings};
    if (settings.find("StdioMode") == std::string_view::npos)
    {
        yaml += "StdioMode: NullDevice\n";
    }

    return yaml;
}

int Configure(process_manager::Process& process, const std::string& yaml)
{
    config_section::ConfigValue value{};
    std::string error{};
    if (config_section::ParseConfigText(yaml, value, error) != config_section::LoadCode::Ok)
    {
        ADD_FAILURE() << "YAML did not parse: " << error;
        return 0;
    }

    config_section::ConfigSection section{value, "test service"};
    return process.UpdateArgs(section);
}

template <typename Predicate>
bool WaitUntil(Predicate done, std::chrono::milliseconds limit = wait_limit)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + limit;
    while (!done())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return done();
        }

        std::this_thread::sleep_for(poll_period);
    }

    return true;
}

bool WaitForExit(process_manager::Process& process)
{
    return WaitUntil([&process]() { return process.ReapExit() > 0; });
}

bool WaitForStop(process_manager::Process& process)
{
    return WaitUntil(
        [&process]()
        {
            process.AdvanceStop();
            return !process.IsStopping();
        },
        stop_limit);
}

bool StopAndWait(process_manager::Process& process)
{
    process.Stop();
    return WaitForStop(process);
}

// The time from just before the reap that sees the exit until NeedsRestart first says yes. The exit time the
// process records is at or after the start of this measure, so the result is never shorter than the real wait.
std::optional<std::chrono::milliseconds> TimeUntilRestartDue(process_manager::Process& process)
{
    Clock::time_point beforeReap{};
    const bool reaped = WaitUntil(
        [&process, &beforeReap]()
        {
            beforeReap = Clock::now();
            return process.ReapExit() > 0;
        });
    if (!reaped)
    {
        return std::nullopt;
    }

    if (!WaitUntil([&process]() { return process.NeedsRestart(); }))
    {
        return std::nullopt;
    }

    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - beforeReap);
}

// Starts a program that ends by itself and tells whether the restart policy wants it back, with no delay.
bool RestartDueAfterExit(std::string_view restartMode, std::string_view program, std::string_view args)
{
    process_manager::Process process{};
    const std::string settings = "Args: [" + std::string{args} + "]\nRestartMode: " + std::string{restartMode} +
                                 "\nRestartDelayMs: 0\nWorkMode: Auto\n";
    if (Configure(process, Service("ender", program, settings)) != 1)
    {
        ADD_FAILURE() << "configuration refused";
        return false;
    }

    process.StartProcess();
    if (!WaitForExit(process))
    {
        ADD_FAILURE() << program << " " << args << " did not exit";
        return false;
    }

    return process.NeedsRestart();
}

bool IsAlive(pid_t pid)
{
    return pid > 0 && ::kill(pid, 0) == 0;
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream file{path};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

int CountOf(const std::string& text, std::string_view part)
{
    int count = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size()))
    {
        ++count;
    }

    return count;
}

bool WaitForText(const std::filesystem::path& path, std::string_view part, int count = 1)
{
    return WaitUntil([&path, part, count]() { return CountOf(ReadFile(path), part) >= count; });
}

// The pid printed right after the n-th (from 1) occurrence of marker, or -1.
pid_t PidAfter(const std::string& text, std::string_view marker, int occurrence = 1)
{
    std::size_t at = std::string::npos;
    std::size_t from = 0;
    for (int seen = 0; seen < occurrence; ++seen)
    {
        at = text.find(marker, from);
        if (at == std::string::npos)
        {
            return -1;
        }

        from = at + marker.size();
    }

    return static_cast<pid_t>(std::strtol(text.c_str() + from, nullptr, 10));
}

class ScratchDirectory
{
public:
    ScratchDirectory()
    {
        const ::testing::TestInfo* test = ::testing::UnitTest::GetInstance()->current_test_info();
        path = std::filesystem::temp_directory_path() /
               ("process_test_" + std::to_string(::getpid()) + "_" + std::string{test != nullptr ? test->name() : "x"});
        std::error_code error{};
        std::filesystem::remove_all(path, error);
        std::filesystem::create_directories(path);
        path = std::filesystem::canonical(path);
    }

    ~ScratchDirectory()
    {
        std::error_code error{};
        std::filesystem::remove_all(path, error);
    }

    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    const std::filesystem::path& Path() const
    {
        return path;
    }

    std::string FileSettings(std::string_view fileName) const
    {
        return "StdioMode: File\nStdioFilePath: " + (path / fileName).string() + "\n";
    }

private:
    std::filesystem::path path;
};

class WorkingDirectory
{
public:
    explicit WorkingDirectory(const std::filesystem::path& path)
        : previous{std::filesystem::current_path()}
    {
        std::filesystem::current_path(path);
    }

    ~WorkingDirectory()
    {
        std::error_code error{};
        std::filesystem::current_path(previous, error);
    }

    WorkingDirectory(const WorkingDirectory&) = delete;
    WorkingDirectory& operator=(const WorkingDirectory&) = delete;

private:
    std::filesystem::path previous;
};

// Whether this machine lets the Process module create its per-service cgroup directory.
bool CgroupsAvailable()
{
    const std::filesystem::path parent = process_manager::ProcessGroupBuilder::GetInstance().GetParentGroupPath();
    const std::filesystem::path probe = parent / ("task_probe_" + std::to_string(::getpid()));
    std::error_code error{};
    const bool created = std::filesystem::create_directories(probe, error);
    if (created)
    {
        std::filesystem::remove(probe, error);
    }

    return created;
}

} // namespace

// ---- Configuration and getters

TEST(ProcessTest, UpdateArgsReturnsOneForCompleteSection)
{
    process_manager::Process process{};
    EXPECT_EQ(Configure(process, Service("complete", "pmt_steady", "")), 1);
    EXPECT_EQ(process.GetProcessName(), "complete");
}

TEST(ProcessTest, UpdateArgsReturnsMinusOneWithoutProcessName)
{
    process_manager::Process process{};
    EXPECT_EQ(Configure(process, "ExecutablePath: " + Program("pmt_steady") + "\n"), -1);
    EXPECT_EQ(process.GetProcessName(), "");
}

TEST(ProcessTest, UpdateArgsReturnsMinusOneWithoutExecutablePath)
{
    process_manager::Process process{};
    EXPECT_EQ(Configure(process, "ProcessName: nopath\n"), -1);
}

TEST(ProcessTest, UpdateArgsKeepsProcessNameWhenItReturnsMinusOneForMissingExecutablePath)
{
    process_manager::Process process{};
    EXPECT_EQ(Configure(process, "ProcessName: half\nDependsOn: [other]\n"), -1);
    EXPECT_EQ(process.GetProcessName(), "half");
    EXPECT_TRUE(process.GetDependencyList().empty());
}

TEST(ProcessTest, UpdateArgsReturnsMinusTwoForReservedNames)
{
    for (const std::string name : {"pm", "all", "BPM", "PMC", "GUI", "a/b"})
    {
        process_manager::Process process{};
        EXPECT_EQ(Configure(process, "ProcessName: " + name + "\nExecutablePath: " + Program("pmt_steady") + "\n"), -2)
            << name;
    }
}

TEST(ProcessTest, ReservedNameIsReportedBeforeMissingExecutablePath)
{
    process_manager::Process process{};
    EXPECT_EQ(Configure(process, "ProcessName: pm\n"), -2);
}

TEST(ProcessTest, ReservedNamesAreCaseSensitiveAndIncludeEmptyName)
{
    EXPECT_TRUE(process_manager::ProcessConfigParams::IsReservedName(""));
    EXPECT_TRUE(process_manager::ProcessConfigParams::IsReservedName("x/y"));
    EXPECT_TRUE(process_manager::ProcessConfigParams::IsReservedName("GUI"));
    EXPECT_FALSE(process_manager::ProcessConfigParams::IsReservedName("PM"));
    EXPECT_FALSE(process_manager::ProcessConfigParams::IsReservedName("gui"));
    EXPECT_FALSE(process_manager::ProcessConfigParams::IsReservedName("service"));
}

TEST(ProcessTest, DependencyListReadsSequenceOrSpaceSeparatedText)
{
    process_manager::Process listed{};
    ASSERT_EQ(Configure(listed, Service("listed", "pmt_steady", "DependsOn: [first, second]\n")), 1);
    EXPECT_EQ(listed.GetDependencyList(), (std::vector<std::string>{"first", "second"}));

    process_manager::Process text{};
    ASSERT_EQ(Configure(text, Service("text", "pmt_steady", "DependsOn: \"first  second\"\n")), 1);
    EXPECT_EQ(text.GetDependencyList(), (std::vector<std::string>{"first", "second"}));

    process_manager::Process none{};
    ASSERT_EQ(Configure(none, Service("none", "pmt_steady", "")), 1);
    EXPECT_TRUE(none.GetDependencyList().empty());
}

TEST(ProcessTest, DefaultConstructedProcessHasNoPidAndNeedsStart)
{
    process_manager::Process process{};
    EXPECT_EQ(process.GetPid(), -1);
    EXPECT_FALSE(process.HasPid());
    EXPECT_TRUE(process.NeedsStart());
    EXPECT_FALSE(process.NeedsRestart());
    EXPECT_EQ(process.GetRuntimeState(), process_manager::RuntimeStateEnum::Unknown);
    EXPECT_FALSE(process.IsRunning());
    EXPECT_FALSE(process.IsStarting());
    EXPECT_FALSE(process.IsStopping());
    EXPECT_FALSE(process.IsRestartRequested());
    EXPECT_FALSE(process.IsGracePeriod());
    EXPECT_TRUE(process.IsWorkingManually());
    EXPECT_EQ(process.ReapExit(), 0);
    EXPECT_EQ(process.GetRuntimeStats().restatCount, 0);
    EXPECT_FALSE(process.IsMember(1, 1, "/sys/fs/cgroup/task_"));
}

TEST(ProcessTest, WorkModeDefaultsToManualAndFollowsConfigAndSetWorkMode)
{
    process_manager::Process unset{};
    ASSERT_EQ(Configure(unset, Service("unset", "pmt_steady", "")), 1);
    EXPECT_TRUE(unset.IsWorkingManually());
    unset.SetWorkMode(process_manager::WorkModeEnum::Auto);
    EXPECT_FALSE(unset.IsWorkingManually());
    unset.SetWorkMode(process_manager::WorkModeEnum::Manual);
    EXPECT_TRUE(unset.IsWorkingManually());

    process_manager::Process automatic{};
    ASSERT_EQ(Configure(automatic, Service("automatic", "pmt_steady", "WorkMode: auto\n")), 1);
    EXPECT_FALSE(automatic.IsWorkingManually());
}

TEST(ProcessTest, CheckConfigAcceptsValidService)
{
    const ScratchDirectory scratch{};
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("valid", "pmt_steady",
                                         "Cwd: " + scratch.Path().string() + "\n" + scratch.FileSettings("out.txt"))),
              1);
    std::vector<std::string> errors{};
    process.CheckConfig(errors);
    EXPECT_TRUE(errors.empty());
}

TEST(ProcessTest, CheckConfigReportsMissingExecutableFiveTimes)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, "ProcessName: svc\nExecutablePath: /nonexistent/pmt_missing\n"), 1);
    std::vector<std::string> errors{"earlier"};
    process.CheckConfig(errors);
    EXPECT_EQ(errors, (std::vector<std::string>{
                          "earlier",
                          "svc ExecutablePath does not exists",
                          "svc ExecutablePath is not a regular file",
                          "svc ExecutablePath access denied",
                          "svc ResolvedPath does not exists",
                          "svc ResolvedPath is not a regular file",
                      }));
}

TEST(ProcessTest, CheckConfigReportsDirectoryAsExecutable)
{
    const ScratchDirectory scratch{};
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, "ProcessName: svc\nExecutablePath: " + scratch.Path().string() + "\n"), 1);
    std::vector<std::string> errors{};
    process.CheckConfig(errors);
    EXPECT_EQ(errors, (std::vector<std::string>{
                          "svc ExecutablePath is a directory",
                          "svc ExecutablePath is not a regular file",
                          "svc ResolvedPath is not a regular file",
                      }));
}

TEST(ProcessTest, CheckConfigReportsMissingCwdTwice)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("svc", "pmt_steady", "Cwd: /nonexistent/pmt\n")), 1);
    std::vector<std::string> errors{};
    process.CheckConfig(errors);
    EXPECT_EQ(errors, (std::vector<std::string>{"svc Cwd does not exists", "svc Cwd is not a directory"}));
}

TEST(ProcessTest, CheckConfigReportsMissingStdioFileDirectory)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("svc", "pmt_steady", "StdioMode: File\nStdioFilePath: /nonexistent/pmt/out.txt\n")),
              1);
    std::vector<std::string> errors{};
    process.CheckConfig(errors);
    EXPECT_EQ(errors, (std::vector<std::string>{"svc StdioFilePath directory does not exists"}));
}

TEST(ProcessTest, CheckConfigReportsEmptyStdioFilePath)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("svc", "pmt_steady", "StdioMode: File\nStdioFilePath: \"\"\n")), 1);
    std::vector<std::string> errors{};
    process.CheckConfig(errors);
    EXPECT_EQ(errors, (std::vector<std::string>{"svc StdioFilePath is empty logging will fail"}));
}

TEST(ProcessTest, CheckConfigIgnoresStdioFilePathOutsideFileMode)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("svc", "pmt_steady", "StdioMode: Pipe\nStdioFilePath: /nonexistent/pmt/out.txt\n")),
              1);
    std::vector<std::string> errors{};
    process.CheckConfig(errors);
    EXPECT_TRUE(errors.empty());
}

TEST(ProcessTest, HealthReportTruncatesNameAndCarriesPidAndState)
{
    const std::string longName(70, 'n');
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service(longName, "pmt_steady", "")), 1);

    const process_manager::DetailedHealthReportStruct idle = process.GetHealthReport();
    EXPECT_EQ(std::string{idle.processName}, std::string(63, 'n'));
    EXPECT_EQ(idle.pid, -1);
    EXPECT_EQ(idle.runtimeHealth.state, process_manager::RuntimeStateEnum::Unknown);
    EXPECT_EQ(idle.gpuUsage.state, process_manager::GpuUsageStateEnum::NotSampled);

    process.StartProcess();
    ASSERT_TRUE(process.HasPid());
    const process_manager::DetailedHealthReportStruct running = process.GetHealthReport();
    EXPECT_EQ(running.pid, process.GetPid());
    EXPECT_EQ(running.runtimeHealth.state, process_manager::RuntimeStateEnum::Running);
    EXPECT_EQ(running.runtimeHealth.restatCount, 0);
    EXPECT_EQ(running.runtimeHealth.missedBeats, 0);
    EXPECT_EQ(running.runtimeHealth.startTime, process.GetRuntimeStats().startTime);
    EXPECT_GE(running.snapshotTime, running.runtimeHealth.startTime);
}

// ---- Start and exit

TEST(ProcessTest, StartGivesRunningProcessWithPidInItsOwnProcessGroup)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("start", "pmt_steady", "")), 1);
    process.StartProcess();
    ASSERT_TRUE(process.HasPid());
    const pid_t pid = process.GetPid();
    EXPECT_GT(pid, 0);
    EXPECT_TRUE(IsAlive(pid));
    EXPECT_EQ(::getpgid(pid), pid);
    EXPECT_TRUE(process.IsRunning());
    EXPECT_FALSE(process.IsStarting());
    EXPECT_EQ(process.GetRuntimeState(), process_manager::RuntimeStateEnum::Running);
    EXPECT_FALSE(process.NeedsStart());
    EXPECT_FALSE(process.NeedsRestart());
    EXPECT_EQ(process.ReapExit(), -1);
    EXPECT_NE(process.GetRuntimeStats().startTime, Clock::time_point{});
    EXPECT_GE(process.GetRuntimeStats().lastSeen, process.GetRuntimeStats().startTime);

    EXPECT_TRUE(process.IsMember(pid, -1, {}));
    EXPECT_TRUE(process.IsMember(pid + 100000, pid, {}));
    EXPECT_FALSE(process.IsMember(pid + 100000, pid + 100000, {}));
    EXPECT_FALSE(process.IsMember(pid + 100000, 0, {}));
}

TEST(ProcessTest, StartAndAutoRestartWhileRunningKeepTheSamePid)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("twice", "pmt_steady", "")), 1);
    process.StartProcess();
    const pid_t pid = process.GetPid();
    process.StartProcess();
    EXPECT_EQ(process.GetPid(), pid);
    process.AutoRestart();
    EXPECT_EQ(process.GetPid(), pid);
    EXPECT_EQ(process.GetRuntimeStats().restatCount, 0);
}

TEST(ProcessTest, CleanExitIsReapedAndShownAsStopped)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("clean", "pmt_exit", "Args: [--code, 0, --after-ms, 0]\nRestartDelayMs: 0\n")), 1);
    process.StartProcess();
    const pid_t pid = process.GetPid();
    ASSERT_GT(pid, 0);

    pid_t reaped = -1;
    ASSERT_TRUE(WaitUntil(
        [&process, &reaped]()
        {
            reaped = process.ReapExit();
            return reaped > 0;
        }));
    EXPECT_EQ(reaped, pid);
    EXPECT_FALSE(process.HasPid());
    EXPECT_EQ(process.GetPid(), -1);
    EXPECT_EQ(process.GetRuntimeState(), process_manager::RuntimeStateEnum::Stopped);
    EXPECT_FALSE(process.IsRunning());
    EXPECT_EQ(process.ReapExit(), 0);
    EXPECT_FALSE(process.NeedsStart());
    EXPECT_FALSE(process.NeedsRestart());
    EXPECT_FALSE(process.IsMember(pid, pid, {}));
    EXPECT_FALSE(IsAlive(pid));
}

TEST(ProcessTest, RestartCountCountsEveryLaunchAfterTheFirst)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("counted", "pmt_exit", "Args: [--code, 0, --after-ms, 0]\n")), 1);
    process.StartProcess();
    EXPECT_EQ(process.GetRuntimeStats().restatCount, 0);
    ASSERT_TRUE(WaitForExit(process));
    process.StartProcess();
    EXPECT_EQ(process.GetRuntimeStats().restatCount, 1);
    ASSERT_TRUE(WaitForExit(process));
    process.AutoRestart();
    EXPECT_EQ(process.GetRuntimeStats().restatCount, 2);
    EXPECT_EQ(process.GetHealthReport().runtimeHealth.restatCount, 2);
}

TEST(ProcessTest, MissingExecutableStartsAsRunningThenExitsAsFailure)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, "ProcessName: missing\nExecutablePath: /nonexistent/pmt_missing\nStdioMode: NullDevice\n"
                                 "RestartDelayMs: 0\n"),
              1);
    process.StartProcess();
    EXPECT_TRUE(process.HasPid());
    EXPECT_TRUE(process.IsRunning());
    ASSERT_TRUE(WaitForExit(process));
    EXPECT_TRUE(process.NeedsRestart());
}

TEST(ProcessTest, UnwritableStdioFileMakesChildExitAsFailure)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("nofile", "pmt_steady",
                                         "StdioMode: File\nStdioFilePath: /nonexistent/pmt/out.txt\nRestartDelayMs: 0\n")),
              1);
    process.StartProcess();
    EXPECT_TRUE(process.HasPid());
    ASSERT_TRUE(WaitForExit(process));
    EXPECT_TRUE(process.NeedsRestart());
}

// ---- Restart policy

TEST(ProcessTest, OnFailureRestartsAfterNonZeroExitCode)
{
    EXPECT_TRUE(RestartDueAfterExit("OnFailure", "pmt_exit", "--code, 3, --after-ms, 0"));
}

TEST(ProcessTest, OnFailureDoesNotRestartAfterExitCodeZero)
{
    EXPECT_FALSE(RestartDueAfterExit("OnFailure", "pmt_exit", "--code, 0, --after-ms, 0"));
}

TEST(ProcessTest, OnFailureRestartsAfterCrashSignals)
{
    for (const std::string signal : {"SEGV", "ABRT", "KILL", "USR1"})
    {
        EXPECT_TRUE(RestartDueAfterExit("OnFailure", "pmt_crash", "--signal, " + signal + ", --after-ms, 0")) << signal;
    }
}

TEST(ProcessTest, OnFailureTreatsTermHupIntAndPipeSignalsAsCleanStop)
{
    for (const std::string signal : {"TERM", "HUP", "INT", "PIPE"})
    {
        EXPECT_FALSE(RestartDueAfterExit("OnFailure", "pmt_crash", "--signal, " + signal + ", --after-ms, 0")) << signal;
    }
}

TEST(ProcessTest, AlwaysRestartsAfterCleanExit)
{
    EXPECT_TRUE(RestartDueAfterExit("Always", "pmt_exit", "--code, 0, --after-ms, 0"));
}

TEST(ProcessTest, NeverDoesNotRestartAfterFailure)
{
    EXPECT_FALSE(RestartDueAfterExit("Never", "pmt_exit", "--code, 3, --after-ms, 0"));
}

TEST(ProcessTest, UnlessStoppedRestartsAfterStopExactlyLikeAlways)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("unless", "pmt_steady", "RestartMode: UnlessStopped\nRestartDelayMs: 0\n")), 1);
    process.StartProcess();
    ASSERT_TRUE(StopAndWait(process));
    EXPECT_FALSE(process.HasPid());
    EXPECT_TRUE(process.NeedsRestart());
}

TEST(ProcessTest, ExitReapedElsewhereCountsAsFailureEvenAfterCleanExit)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("stolen", "pmt_exit", "Args: [--code, 0, --after-ms, 0]\nRestartDelayMs: 0\n")), 1);
    process.StartProcess();
    const pid_t pid = process.GetPid();
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    EXPECT_EQ(process.ReapExit(), pid);
    EXPECT_FALSE(process.HasPid());
    EXPECT_EQ(process.GetRuntimeState(), process_manager::RuntimeStateEnum::Stopped);
    EXPECT_TRUE(process.NeedsRestart());
}

TEST(ProcessTest, RestartDelayGrowsByBackoffMultiplierOnEachRestartInARow)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("backoff", "pmt_exit",
                                         "Args: [--code, 3, --after-ms, 0]\nRestartDelayMs: 100\nBackoffMultiplier: 2\n"
                                         "MaxRestarts: 5\n")),
              1);
    process.StartProcess();
    std::vector<std::chrono::milliseconds> waited{};
    for (int restart = 0; restart < 3; ++restart)
    {
        const std::optional<std::chrono::milliseconds> wait = TimeUntilRestartDue(process);
        ASSERT_TRUE(wait.has_value()) << "restart " << restart;
        waited.push_back(*wait);
        process.AutoRestart();
        ASSERT_TRUE(process.HasPid());
    }

    const std::vector<std::chrono::milliseconds> expected{std::chrono::milliseconds{100}, std::chrono::milliseconds{200},
                                                          std::chrono::milliseconds{400}};
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_GE(waited[i], expected[i]) << "restart " << i;
        EXPECT_LT(waited[i], expected[i] * 2) << "restart " << i;
    }

    EXPECT_EQ(process.GetRuntimeStats().restatCount, 3);
}

TEST(ProcessTest, RestartLimitStopsAutomaticRestarts)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("limited", "pmt_exit",
                                         "Args: [--code, 3, --after-ms, 0]\nRestartDelayMs: 10\nBackoffMultiplier: 1\n"
                                         "MaxRestarts: 2\nWorkMode: Auto\n")),
              1);
    process.StartProcess();
    for (int restart = 0; restart < 2; ++restart)
    {
        ASSERT_TRUE(TimeUntilRestartDue(process).has_value()) << "restart " << restart;
        process.AutoRestart();
    }

    ASSERT_TRUE(WaitForExit(process));
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    EXPECT_FALSE(process.NeedsRestart());
    EXPECT_EQ(process.GetRuntimeStats().restatCount, 2);
}

TEST(ProcessTest, ManualStartRearmsRestartLimit)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("rearmed", "pmt_exit",
                                         "Args: [--code, 3, --after-ms, 0]\nRestartDelayMs: 10\nBackoffMultiplier: 1\n"
                                         "MaxRestarts: 1\n")),
              1);
    process.StartProcess();
    ASSERT_TRUE(TimeUntilRestartDue(process).has_value());
    process.AutoRestart();
    ASSERT_TRUE(WaitForExit(process));
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    ASSERT_FALSE(process.NeedsRestart());

    process.StartProcess();
    EXPECT_TRUE(TimeUntilRestartDue(process).has_value());
}

TEST(ProcessTest, ZeroMaxRestartsNeverRestarts)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("zero", "pmt_exit",
                                         "Args: [--code, 3, --after-ms, 0]\nRestartDelayMs: 0\nMaxRestarts: 0\n")),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForExit(process));
    EXPECT_FALSE(process.NeedsRestart());
}

TEST(ProcessTest, RunAsLongAsRestartResetMsStartsANewRestartSeries)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("reset", "pmt_exit",
                                         "Args: [--code, 3, --after-ms, 0]\nRestartDelayMs: 0\nMaxRestarts: 1\n"
                                         "RestartResetMs: 0\n")),
              1);
    process.StartProcess();
    for (int restart = 0; restart < 3; ++restart)
    {
        ASSERT_TRUE(WaitForExit(process)) << "restart " << restart;
        EXPECT_TRUE(process.NeedsRestart()) << "restart " << restart;
        process.AutoRestart();
    }

    EXPECT_EQ(process.GetRuntimeStats().restatCount, 3);
}

// ---- Stopping

TEST(ProcessTest, StopWithoutPidDoesNothing)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("idle", "pmt_steady", "")), 1);
    process.Stop();
    EXPECT_FALSE(process.IsStopping());
    process.RequestStopAndEscalate();
    EXPECT_FALSE(process.IsStopping());
    process.AdvanceStop();
    EXPECT_EQ(process.GetRuntimeState(), process_manager::RuntimeStateEnum::Unknown);
}

TEST(ProcessTest, StopEndsServiceThatHonoursSigtermWithoutForceKill)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("graceful", "pmt_steady",
                                         "Args: [--stop-delay-ms, 100]\nForceKillTimeoutMs: 5000\nRestartDelayMs: 0\n")),
              1);
    process.StartProcess();
    const pid_t pid = process.GetPid();
    ASSERT_GT(pid, 0);

    const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    process.Stop();
    EXPECT_TRUE(process.IsStopping());
    EXPECT_FALSE(process.IsRestartRequested());
    ASSERT_TRUE(WaitForStop(process));
    const std::chrono::steady_clock::duration took = std::chrono::steady_clock::now() - begin;

    EXPECT_LT(took, std::chrono::milliseconds{4000});
    EXPECT_FALSE(process.HasPid());
    EXPECT_FALSE(IsAlive(pid));
    EXPECT_EQ(process.GetRuntimeState(), process_manager::RuntimeStateEnum::Stopped);
    EXPECT_FALSE(process.NeedsRestart());
}

TEST(ProcessTest, StopKillsServiceAndChildIgnoringSigtermAfterForceKillTimeout)
{
    const ScratchDirectory scratch{};
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("stubborn", "pmt_stubborn",
                                         "Args: [--child, --period-ms, 50]\nForceKillTimeoutMs: 1000\nRestartDelayMs: 0\n" +
                                             scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    const pid_t pid = process.GetPid();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "child tick"));
    const pid_t child = PidAfter(ReadFile(scratch.Path() / "out.txt"), "started child ");
    ASSERT_GT(child, 0);

    const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    process.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    process.AdvanceStop();
    EXPECT_TRUE(process.IsStopping());
    EXPECT_TRUE(process.HasPid());
    EXPECT_TRUE(IsAlive(child));

    ASSERT_TRUE(WaitForStop(process));
    const std::chrono::steady_clock::duration took = std::chrono::steady_clock::now() - begin;
    EXPECT_GE(took, std::chrono::milliseconds{1000});
    EXPECT_FALSE(process.HasPid());
    EXPECT_FALSE(IsAlive(pid));
    EXPECT_FALSE(IsAlive(child));
    EXPECT_GE(CountOf(ReadFile(scratch.Path() / "out.txt"), "ignored SIGTERM #1"), 2);
    // Killed by SIGKILL, which OnFailure counts as a failure.
    EXPECT_TRUE(process.NeedsRestart());
}

TEST(ProcessTest, ForceKillTimeoutBelowOneSecondIsRaisedToOneSecond)
{
    const ScratchDirectory scratch{};
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("quick", "pmt_stubborn",
                                         "Args: [--period-ms, 50]\nForceKillTimeoutMs: 1\n" + scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "parent tick"));

    const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    ASSERT_TRUE(StopAndWait(process));
    EXPECT_GE(std::chrono::steady_clock::now() - begin, std::chrono::milliseconds{1000});
}

TEST(ProcessTest, StopKillsChildrenInTheServiceProcessGroup)
{
    const ScratchDirectory scratch{};
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("forker", "pmt_forker",
                                         "Args: [--children, 2, --period-ms, 50, --child-lifetime-ms, 5000]\n" +
                                             scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    const std::filesystem::path out = scratch.Path() / "out.txt";
    ASSERT_TRUE(WaitForText(out, "started child ", 2));
    const std::string text = ReadFile(out);
    const pid_t first = PidAfter(text, "started child ", 1);
    const pid_t second = PidAfter(text, "started child ", 2);
    ASSERT_TRUE(IsAlive(first));
    ASSERT_TRUE(IsAlive(second));
    EXPECT_EQ(::getpgid(first), process.GetPid());

    ASSERT_TRUE(StopAndWait(process));
    EXPECT_FALSE(IsAlive(first));
    EXPECT_FALSE(IsAlive(second));
}

TEST(ProcessTest, WithoutCgroupsStopLeavesSetsidEscapeeRunning)
{
    if (CgroupsAvailable())
    {
        GTEST_SKIP() << "cgroups are available here; this pins the path without them";
    }

    const ScratchDirectory scratch{};
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("escape", "pmt_forker",
                                         "Args: [--children, 1, --escapee, --period-ms, 50, --child-lifetime-ms, 5000]\n" +
                                             scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    const std::filesystem::path out = scratch.Path() / "out.txt";
    ASSERT_TRUE(WaitForText(out, "escapee tick"));
    const std::string text = ReadFile(out);
    const pid_t child = PidAfter(text, "started child ");
    const pid_t escapee = PidAfter(text, "started escapee ");
    ASSERT_GT(escapee, 0);

    ASSERT_TRUE(StopAndWait(process));
    EXPECT_FALSE(IsAlive(child));
    EXPECT_TRUE(IsAlive(escapee));
    ::kill(escapee, SIGKILL);
}

TEST(ProcessTest, ChildrenOutliveLeaderThatExitsOnItsOwnAndStopCannotReachThem)
{
    const ScratchDirectory scratch{};
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("orphans", "pmt_forker",
                                         "Args: [--children, 1, --crash-parent-after-ms, 200, --period-ms, 50, "
                                         "--child-lifetime-ms, 5000]\n" +
                                             scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    const pid_t leader = process.GetPid();
    ASSERT_TRUE(WaitForExit(process));
    const pid_t child = PidAfter(ReadFile(scratch.Path() / "out.txt"), "started child ");
    ASSERT_GT(child, 0);

    EXPECT_TRUE(IsAlive(child));
    process.Stop();
    EXPECT_FALSE(process.IsStopping());
    EXPECT_TRUE(IsAlive(child));
    ::kill(-leader, SIGKILL);
}

TEST(ProcessTest, RestartRequestSurvivesTheStopUntilTheNextStart)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("restart", "pmt_steady", "")), 1);
    process.StartProcess();
    const pid_t first = process.GetPid();

    process.Restart();
    EXPECT_TRUE(process.IsRestartRequested());
    EXPECT_TRUE(process.IsStopping());
    ASSERT_TRUE(WaitForStop(process));
    EXPECT_FALSE(process.HasPid());
    EXPECT_TRUE(process.IsRestartRequested());

    process.StartProcess();
    EXPECT_TRUE(process.HasPid());
    EXPECT_NE(process.GetPid(), first);
    EXPECT_FALSE(process.IsRestartRequested());
}

TEST(ProcessTest, StopDuringRestartClearsTheRestartRequest)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("cancel", "pmt_steady", "")), 1);
    process.StartProcess();
    process.Restart();
    process.Stop();
    EXPECT_FALSE(process.IsRestartRequested());
    EXPECT_TRUE(process.IsStopping());
    ASSERT_TRUE(WaitForStop(process));
    EXPECT_FALSE(process.IsRestartRequested());
}

TEST(ProcessTest, RequestStopAndEscalateBlocksUntilStopped)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("blocking", "pmt_steady", "")), 1);
    process.StartProcess();
    const pid_t pid = process.GetPid();
    process.RequestStopAndEscalate(std::chrono::milliseconds{10});
    EXPECT_FALSE(process.IsStopping());
    EXPECT_FALSE(process.HasPid());
    EXPECT_FALSE(IsAlive(pid));
    EXPECT_EQ(process.GetRuntimeState(), process_manager::RuntimeStateEnum::Stopped);
}

TEST(ProcessTest, DestructorStopsTheRunningChild)
{
    pid_t pid = -1;
    {
        process_manager::Process process{};
        ASSERT_EQ(Configure(process, Service("scoped", "pmt_steady", "")), 1);
        process.StartProcess();
        pid = process.GetPid();
        ASSERT_TRUE(IsAlive(pid));
    }

    EXPECT_FALSE(IsAlive(pid));
}

TEST(ProcessTest, MoveConstructionHandsTheChildOver)
{
    process_manager::Process source{};
    ASSERT_EQ(Configure(source, Service("moved", "pmt_steady", "")), 1);
    source.StartProcess();
    const pid_t pid = source.GetPid();

    process_manager::Process target{std::move(source)};
    EXPECT_FALSE(source.HasPid());
    EXPECT_EQ(source.GetPid(), -1);
    EXPECT_EQ(target.GetPid(), pid);
    EXPECT_EQ(target.GetProcessName(), "moved");
    EXPECT_TRUE(target.IsRunning());
    EXPECT_TRUE(IsAlive(pid));
}

TEST(ProcessTest, MoveAssignmentStopsTheChildItReplaces)
{
    process_manager::Process source{};
    ASSERT_EQ(Configure(source, Service("incoming", "pmt_steady", "")), 1);
    source.StartProcess();
    const pid_t incoming = source.GetPid();

    process_manager::Process target{};
    ASSERT_EQ(Configure(target, Service("replaced", "pmt_steady", "")), 1);
    target.StartProcess();
    const pid_t replaced = target.GetPid();

    target = std::move(source);
    EXPECT_FALSE(IsAlive(replaced));
    EXPECT_TRUE(IsAlive(incoming));
    EXPECT_EQ(target.GetPid(), incoming);
    EXPECT_EQ(target.GetProcessName(), "incoming");
    EXPECT_FALSE(source.HasPid());
}

// ---- What reaches the child

TEST(ProcessTest, ArgsListKeepsItemWithSpacesInOnePiece)
{
    const ScratchDirectory scratch{};
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("args", "pmt_env", "Args: [--show, HOME, two words, plain]\n" +
                                                               scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "env HOME"));
    const std::string text = ReadFile(scratch.Path() / "out.txt");
    EXPECT_NE(text.find("argc 5"), std::string::npos) << text;
    EXPECT_NE(text.find("argv[1]='--show'"), std::string::npos) << text;
    EXPECT_NE(text.find("argv[3]='two words'"), std::string::npos) << text;
    EXPECT_NE(text.find("argv[4]='plain'"), std::string::npos) << text;
}

TEST(ProcessTest, ArgsTextIsSplitAtSpaces)
{
    const ScratchDirectory scratch{};
    process_manager::Process process{};
    ASSERT_EQ(Configure(process,
                        Service("args", "pmt_env", "Args: --show HOME two words\n" + scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "env HOME"));
    const std::string text = ReadFile(scratch.Path() / "out.txt");
    EXPECT_NE(text.find("argc 5"), std::string::npos) << text;
    EXPECT_NE(text.find("argv[3]='two'"), std::string::npos) << text;
    EXPECT_NE(text.find("argv[4]='words'"), std::string::npos) << text;
}

TEST(ProcessTest, OverrideStartupArgsReplacesConfiguredArgs)
{
    const ScratchDirectory scratch{};
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("override", "pmt_env", "Args: [--show, HOME, extra]\n" +
                                                                   scratch.FileSettings("out.txt"))),
              1);
    process.OverrideStartupArgs({"--show", "PMT_OVERRIDE"});
    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "env PMT_OVERRIDE"));
    const std::string text = ReadFile(scratch.Path() / "out.txt");
    EXPECT_NE(text.find("argc 3"), std::string::npos) << text;
    EXPECT_NE(text.find("argv[2]='PMT_OVERRIDE'"), std::string::npos) << text;
}

TEST(ProcessTest, EnvironmentVariablesAreAddedToTheInheritedEnvironment)
{
    const ScratchDirectory scratch{};
    ASSERT_EQ(::setenv("PMT_INHERITED", "yes", 1), 0);
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("env", "pmt_env",
                                         "Args: [--show, PMT_COLOR, --show, PMT_EMPTY, --show, PMT_INHERITED]\n"
                                         "EnviromentVariables: {PMT_COLOR: blue, PMT_EMPTY: \"\"}\n" +
                                             scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "env PMT_INHERITED"));
    ::unsetenv("PMT_INHERITED");
    const std::string text = ReadFile(scratch.Path() / "out.txt");
    EXPECT_NE(text.find("env PMT_COLOR='blue'"), std::string::npos) << text;
    EXPECT_NE(text.find("env PMT_EMPTY=''"), std::string::npos) << text;
    EXPECT_NE(text.find("env PMT_INHERITED='yes'"), std::string::npos) << text;
}

TEST(ProcessTest, ClearedEnvironmentHoldsOnlyTheConfiguredVariables)
{
    const ScratchDirectory scratch{};
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("cleared", "pmt_env",
                                         "Args: [--all-env]\nInheritParentEnviroment: false\n"
                                         "EnviromentVariables: PMT_ONLY=this\n" +
                                             scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "environ "));
    const std::string text = ReadFile(scratch.Path() / "out.txt");
    EXPECT_NE(text.find("environment has 1 variables"), std::string::npos) << text;
    EXPECT_NE(text.find("environ PMT_ONLY=this"), std::string::npos) << text;
    EXPECT_NE(text.find("env PATH unset"), std::string::npos) << text;
}

TEST(ProcessTest, UseDisplaySetsDisplayAndXauthority)
{
    const ScratchDirectory scratch{};
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("display", "pmt_env",
                                         "UseDisplay: true\nDisplay: \":7\"\nXAuthority: /tmp/pmt.Xauthority\n" +
                                             scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "env JOURNAL_STREAM"));
    const std::string text = ReadFile(scratch.Path() / "out.txt");
    EXPECT_NE(text.find("env DISPLAY=':7'"), std::string::npos) << text;
    EXPECT_NE(text.find("env XAUTHORITY='/tmp/pmt.Xauthority'"), std::string::npos) << text;
}

TEST(ProcessTest, CwdBecomesTheChildWorkingDirectory)
{
    const ScratchDirectory scratch{};
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("cwd", "pmt_env",
                                         "Cwd: " + scratch.Path().string() + "\n" + scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "cwd "));
    const std::string text = ReadFile(scratch.Path() / "out.txt");
    EXPECT_NE(text.find("cwd " + scratch.Path().string() + "\n"), std::string::npos) << text;
}

TEST(ProcessTest, MissingCwdStillStartsTheChildInTheManagerDirectory)
{
    const ScratchDirectory scratch{};
    const WorkingDirectory here{scratch.Path()};
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("nocwd", "pmt_env", "Cwd: /nonexistent/pmt\n" + scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "cwd "));
    const std::string text = ReadFile(scratch.Path() / "out.txt");
    EXPECT_NE(text.find("cwd " + scratch.Path().string() + "\n"), std::string::npos) << text;
    EXPECT_TRUE(process.HasPid());
}

TEST(ProcessTest, StdioFileIsTruncatedOnEachStartByDefault)
{
    const ScratchDirectory scratch{};
    const std::filesystem::path out = scratch.Path() / "out.txt";
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("truncated", "pmt_env", scratch.FileSettings("out.txt"))), 1);
    process.StartProcess();
    ASSERT_TRUE(WaitForText(out, "env JOURNAL_STREAM"));
    ASSERT_TRUE(StopAndWait(process));

    process.StartProcess();
    const std::string second = "pid " + std::to_string(process.GetPid()) + " ";
    ASSERT_TRUE(WaitForText(out, second));
    EXPECT_EQ(CountOf(ReadFile(out), "argc "), 1);
}

TEST(ProcessTest, StdioAppendKeepsEarlierOutput)
{
    const ScratchDirectory scratch{};
    const std::filesystem::path out = scratch.Path() / "out.txt";
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("appended", "pmt_env", "StdioAppend: true\n" + scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForText(out, "env JOURNAL_STREAM"));
    ASSERT_TRUE(StopAndWait(process));

    process.StartProcess();
    ASSERT_TRUE(WaitForText(out, "env JOURNAL_STREAM", 2));
    EXPECT_EQ(CountOf(ReadFile(out), "argc "), 2);
}

TEST(ProcessTest, PipeOutputIsLoggedLineByLineUnderTheServiceName)
{
    const ScratchDirectory scratch{};
    const std::filesystem::path logPath = scratch.Path() / "log.txt";
    std::FILE* log = std::fopen(logPath.c_str(), "w");
    ASSERT_NE(log, nullptr);
    logger::Logger::Instance().SetConsole(log, false);

    process_manager::Process process{};
    const int configured = Configure(process, Service("piped", "pmt_env", "Args: [--show, PMT_PIPE]\nStdioMode: Pipe\n"));
    process.StartProcess();
    const bool logged = WaitUntil(
        [&process, &logPath]()
        {
            process.DrainStdioPipe();
            return ReadFile(logPath).find("env PMT_PIPE unset") != std::string::npos;
        });
    process.RequestStopAndEscalate(std::chrono::milliseconds{10});

    logger::Logger::Instance().SetConsole(nullptr, false);
    std::fclose(log);

    EXPECT_EQ(configured, 1);
    ASSERT_TRUE(logged);
    const std::string text = ReadFile(logPath);
    std::istringstream lines{text};
    std::string line{};
    bool found = false;
    while (std::getline(lines, line))
    {
        if (line.find("argv[1]='--show'") != std::string::npos)
        {
            found = true;
            EXPECT_NE(line.find("[piped] "), std::string::npos) << line;
            EXPECT_NE(line.find("pmt_env["), std::string::npos) << line;
        }
    }

    EXPECT_TRUE(found) << text;
}

// ---- Health and heartbeats, driven directly without the broker

TEST(ProcessTest, HealthCheckStartsInStartingStateUntilFirstHeartbeat)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("health", "pmt_steady", "CheckHealth: true\nGracePeriodMs: 2000\n")), 1);
    process.StartProcess();
    EXPECT_TRUE(process.IsStarting());
    EXPECT_FALSE(process.IsRunning());
    EXPECT_EQ(process.GetRuntimeState(), process_manager::RuntimeStateEnum::Starting);

    process.HeartbeatReceived();
    EXPECT_TRUE(process.IsRunning());
    EXPECT_FALSE(process.IsStarting());
    EXPECT_EQ(process.GetRuntimeStats().missedBeats, 0);
}

TEST(ProcessTest, HeartbeatWithoutHealthCheckChangesNothing)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("nohealth", "pmt_steady", "GracePeriodMs: 0\nHeartbeatPeriodMs: 1\n")), 1);
    process.StartProcess();
    const process_manager::RuntimeHealthStruct before = process.GetRuntimeStats();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    process.HeartbeatReceived();
    process.CheckHeartbeat();
    EXPECT_EQ(process.GetRuntimeState(), process_manager::RuntimeStateEnum::Running);
    EXPECT_EQ(process.GetRuntimeStats().lastSeen, before.lastSeen);
    EXPECT_EQ(process.GetRuntimeStats().missedBeats, 0);
}

TEST(ProcessTest, GracePeriodEndsAfterGracePeriodMs)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("grace", "pmt_steady", "GracePeriodMs: 100\n")), 1);
    const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    process.StartProcess();
    EXPECT_TRUE(process.IsGracePeriod());
    ASSERT_TRUE(WaitUntil([&process]() { return !process.IsGracePeriod(); }));
    EXPECT_GE(std::chrono::steady_clock::now() - begin, std::chrono::milliseconds{100});
}

TEST(ProcessTest, ZeroGracePeriodMeansNoGracePeriod)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("nograce", "pmt_steady", "GracePeriodMs: 0\n")), 1);
    process.StartProcess();
    EXPECT_FALSE(process.IsGracePeriod());
}

TEST(ProcessTest, CheckHeartbeatDoesNothingDuringGracePeriod)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("graceful", "pmt_steady",
                                         "CheckHealth: true\nGracePeriodMs: 5000\nHeartbeatPeriodMs: 1\n"
                                         "MissedHeartbeatThreshold: 1\n")),
              1);
    process.StartProcess();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    process.CheckHeartbeat();
    EXPECT_TRUE(process.IsGracePeriod());
    EXPECT_TRUE(process.IsStarting());
    EXPECT_EQ(process.GetRuntimeStats().missedBeats, 0);
    EXPECT_FALSE(process.NeedsRestart());
}

TEST(ProcessTest, MissedHeartbeatsAfterGracePeriodMakeServiceUnhealthy)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("silent", "pmt_steady",
                                         "CheckHealth: true\nGracePeriodMs: 0\nHeartbeatPeriodMs: 20\n"
                                         "MissedHeartbeatThreshold: 3\n")),
              1);
    const Clock::time_point begin = Clock::now();
    process.StartProcess();
    process.CheckHeartbeat();
    EXPECT_TRUE(process.IsStarting());
    ASSERT_TRUE(WaitUntil(
        [&process]()
        {
            process.CheckHeartbeat();
            return process.GetRuntimeState() == process_manager::RuntimeStateEnum::Unhealthy;
        }));
    EXPECT_GE(Clock::now() - begin, std::chrono::milliseconds{60});
    EXPECT_GE(process.GetRuntimeStats().missedBeats, 3);
    EXPECT_FALSE(process.IsRunning());
    EXPECT_FALSE(process.IsStarting());
    EXPECT_TRUE(process.HasPid());
}

TEST(ProcessTest, HeartbeatBringsUnhealthyServiceBackToRunning)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("flaky", "pmt_steady",
                                         "CheckHealth: true\nGracePeriodMs: 0\nHeartbeatPeriodMs: 10\n"
                                         "MissedHeartbeatThreshold: 1\n")),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitUntil(
        [&process]()
        {
            process.CheckHeartbeat();
            return process.GetRuntimeState() == process_manager::RuntimeStateEnum::Unhealthy;
        }));
    const Clock::time_point lastSeenBefore = process.GetRuntimeStats().lastSeen;

    process.HeartbeatReceived();
    EXPECT_TRUE(process.IsRunning());
    EXPECT_EQ(process.GetRuntimeStats().missedBeats, 0);
    EXPECT_GT(process.GetRuntimeStats().lastSeen, lastSeenBefore);
    EXPECT_FALSE(process.NeedsRestart());
}

TEST(ProcessTest, UnhealthyServiceNeedsRestartAndItsStopCountsAsFailureDespiteCleanExit)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("unresponsive", "pmt_steady",
                                         "CheckHealth: true\nGracePeriodMs: 0\nHeartbeatPeriodMs: 10\n"
                                         "MissedHeartbeatThreshold: 2\nRestartDelayMs: 0\n")),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitUntil(
        [&process]()
        {
            process.CheckHeartbeat();
            return process.GetRuntimeState() == process_manager::RuntimeStateEnum::Unhealthy;
        }));
    EXPECT_TRUE(process.NeedsRestart());

    process.StopUnresponsive();
    EXPECT_TRUE(process.IsStopping());
    EXPECT_FALSE(process.IsRestartRequested());
    ASSERT_TRUE(WaitForStop(process));
    EXPECT_EQ(process.GetRuntimeState(), process_manager::RuntimeStateEnum::Stopped);
    EXPECT_TRUE(process.NeedsRestart());

    process.AutoRestart();
    EXPECT_TRUE(process.HasPid());
    EXPECT_TRUE(process.IsStarting());
    EXPECT_EQ(process.GetRuntimeStats().missedBeats, 0);
}

TEST(ProcessTest, NeverModeDoesNotRestartUnhealthyService)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("ignored", "pmt_steady",
                                         "CheckHealth: true\nGracePeriodMs: 0\nHeartbeatPeriodMs: 10\n"
                                         "MissedHeartbeatThreshold: 1\nRestartMode: Never\n")),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitUntil(
        [&process]()
        {
            process.CheckHeartbeat();
            return process.GetRuntimeState() == process_manager::RuntimeStateEnum::Unhealthy;
        }));
    EXPECT_FALSE(process.NeedsRestart());
}

TEST(ProcessTest, SuspendedHealthCheckIgnoresHeartbeatsUntilNextStart)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("suspended", "pmt_steady",
                                         "CheckHealth: true\nGracePeriodMs: 0\nHeartbeatPeriodMs: 1\n"
                                         "MissedHeartbeatThreshold: 1\n")),
              1);
    process.StartProcess();
    process.SuspendHealthCheck();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    process.CheckHeartbeat();
    EXPECT_TRUE(process.IsStarting());
    EXPECT_EQ(process.GetRuntimeStats().missedBeats, 0);
    process.HeartbeatReceived();
    EXPECT_TRUE(process.IsStarting());
    EXPECT_FALSE(process.NeedsRestart());

    ASSERT_TRUE(StopAndWait(process));
    process.StartProcess();
    process.HeartbeatReceived();
    EXPECT_TRUE(process.IsRunning());
}

TEST(ProcessTest, HeartbeatsAfterExitLeaveServiceStopped)
{
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("late", "pmt_steady",
                                         "CheckHealth: true\nGracePeriodMs: 0\nHeartbeatPeriodMs: 1\n"
                                         "MissedHeartbeatThreshold: 1\n")),
              1);
    process.StartProcess();
    ASSERT_TRUE(StopAndWait(process));
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    process.HeartbeatReceived();
    EXPECT_EQ(process.GetRuntimeState(), process_manager::RuntimeStateEnum::Stopped);
    process.CheckHeartbeat();
    EXPECT_EQ(process.GetRuntimeState(), process_manager::RuntimeStateEnum::Stopped);
    EXPECT_EQ(process.GetRuntimeStats().missedBeats, 0);
}

// ---- Without cgroups (WSL1, containers, no systemd delegation)

TEST(ProcessTest, WithoutCgroupsUsageIsZeroAndCgroupMembershipIsIgnored)
{
    if (CgroupsAvailable())
    {
        GTEST_SKIP() << "cgroups are available here; this pins the path without them";
    }

    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("nocgroup", "pmt_steady", "")), 1);
    process.StartProcess();
    ASSERT_TRUE(process.HasPid());
    EXPECT_TRUE(process.IsRunning());

    const std::filesystem::path own =
        process_manager::ProcessGroupBuilder::GetInstance().GetParentGroupPath() / "task_nocgroup";
    EXPECT_FALSE(std::filesystem::exists(own));
    EXPECT_FALSE(process.IsMember(1, 1, own / "child"));
    EXPECT_EQ(process.GetMemoryUsageInBytes(), 0u);
    EXPECT_EQ(process.GetCpuUsageInUsec(), 0u);
    EXPECT_EQ(process.GetHealthReport().memoryUsageInBytes, 0u);
    EXPECT_EQ(process.GetHealthReport().cpuUsageInUsec, 0u);

    ASSERT_TRUE(StopAndWait(process));
    process.StartProcess();
    EXPECT_TRUE(process.HasPid());
}

TEST(ProcessTest, UsageBeforeFirstStartIsReadRelativeToTheWorkingDirectory)
{
    const ScratchDirectory scratch{};
    {
        std::ofstream memory{scratch.Path() / "memory.current"};
        memory << "4242\n";
        std::ofstream cpu{scratch.Path() / "cpu.stat"};
        cpu << "usage_usec 777\nuser_usec 1\n";
    }

    const WorkingDirectory here{scratch.Path()};
    process_manager::Process process{};
    ASSERT_EQ(Configure(process, Service("relative", "pmt_steady", "")), 1);
    EXPECT_EQ(process.GetMemoryUsageInBytes(), 4242u);
    EXPECT_EQ(process.GetCpuUsageInUsec(), 777u);
}
