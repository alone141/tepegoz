#include <gtest/gtest.h>

#include "Process.hpp"

#include "ConfigSection.hpp"
#include "Logger.hpp"
#include "ProcessGroupBuilder.hpp"

#include <atomic>
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
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds poll_period{5};
constexpr std::chrono::milliseconds wait_limit{3000};
constexpr std::chrono::milliseconds stop_limit{5000};
// How long a process that must not be signalled is watched.
constexpr std::chrono::milliseconds unharmed_limit{200};

std::string Program(std::string_view name)
{
    return std::string{TPZ_BIN_DIR} + "/" + std::string{name};
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

int Configure(tepegoz::Process& process, const std::string& yaml)
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

bool WaitForExit(tepegoz::Process& process)
{
    return WaitUntil([&process]() { return process.ReapExit() > 0; });
}

bool WaitForStop(tepegoz::Process& process)
{
    return WaitUntil(
        [&process]()
        {
            process.AdvanceStop();
            return !process.IsStopping();
        },
        stop_limit);
}

bool StopAndWait(tepegoz::Process& process)
{
    process.Stop();
    return WaitForStop(process);
}

// The time from just before the reap that sees the exit until NeedsRestart first says yes. The exit time the
// process records is at or after the start of this measure, so the result is never shorter than the real wait.
std::optional<std::chrono::milliseconds> TimeUntilRestartDue(tepegoz::Process& process)
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
    tepegoz::Process process{};
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

// Starts a program that exits with 0 at once, reaps it outside the Process as another waitpid in the manager would,
// and tells whether the restart policy wants it back, with no delay.
bool RestartDueAfterCleanExitReapedElsewhere(std::string_view restartMode)
{
    tepegoz::Process process{};
    const std::string settings = "Args: [--code, 0, --after-ms, 0]\nRestartMode: " + std::string{restartMode} +
                                 "\nRestartDelayMs: 0\n";
    if (Configure(process, Service("stolen", "tpz_exit", settings)) != 1)
    {
        ADD_FAILURE() << "configuration refused";
        return false;
    }

    process.StartProcess();
    const pid_t pid = process.GetPid();
    int status = 0;
    if (pid <= 0 || ::waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        ADD_FAILURE() << "tpz_exit was not reaped with exit code 0";
        return false;
    }

    EXPECT_EQ(process.ReapExit(), pid);
    EXPECT_FALSE(process.HasPid());
    EXPECT_EQ(process.GetRuntimeState(), tepegoz::RuntimeState::Stopped);
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
    const std::filesystem::path parent = tepegoz::ProcessGroupBuilder::GetInstance().GetParentGroupPath();
    const std::filesystem::path probe = parent / ("task_probe_" + std::to_string(::getpid()));
    std::error_code error{};
    const bool created = std::filesystem::create_directories(probe, error);
    if (created)
    {
        std::filesystem::remove(probe, error);
    }

    return created;
}

void WriteText(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream file{path};
    file << text;
}

// Makes the cgroup.procs of a folder that stands in for a cgroup list these pids, as the kernel lists the members.
void ListMembers(const std::filesystem::path& cgroup, const std::vector<pid_t>& pids)
{
    std::string text{};
    for (const pid_t pid : pids)
    {
        text += std::to_string(pid) + "\n";
    }

    WriteText(cgroup / "cgroup.procs", text);
}

// A process of the test's own in a session of its own, as a service's child that called setsid() would be: no
// service's process group holds it. It only waits, and ends by itself after 30 s if nothing ends it sooner.
class Bystander
{
public:
    Bystander()
        : pid{::fork()}
    {
        if (pid == 0)
        {
            ::setsid();
            ::alarm(30);
            for (;;)
            {
                ::pause();
            }
        }
    }

    ~Bystander()
    {
        if (pid > 0 && !reaped)
        {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
        }
    }

    Bystander(const Bystander&) = delete;
    Bystander& operator=(const Bystander&) = delete;

    pid_t Pid() const
    {
        return pid;
    }

    // Whether SIGKILL ended it within the limit.
    bool WasKilled(std::chrono::milliseconds limit = wait_limit)
    {
        if (pid <= 0)
        {
            return false;
        }

        if (!reaped)
        {
            reaped = WaitUntil([this]() { return ::waitpid(pid, &status, WNOHANG) == pid; }, limit);
        }

        return reaped && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
    }

private:
    pid_t pid;
    int status = 0;
    bool reaped = false;
};

// While it lives, a folder stands in for the manager's cgroup, so Process creates task_<name> in it. A real cgroup's
// files are the kernel's. Here cgroup.procs is an ordinary file: the service's child writes its pid into it when it
// joins, and the test writes what else the cgroup is to hold.
class FakeCgroupParent
{
public:
    FakeCgroupParent()
        : previous{tepegoz::ProcessGroupBuilder::GetInstance().GetParentGroupPath()}
    {
        tepegoz::ProcessGroupBuilder::GetInstance().SetParentGroupPath(scratch.Path());
    }

    ~FakeCgroupParent()
    {
        tepegoz::ProcessGroupBuilder::GetInstance().SetParentGroupPath(previous);
    }

    FakeCgroupParent(const FakeCgroupParent&) = delete;
    FakeCgroupParent& operator=(const FakeCgroupParent&) = delete;

    // The cgroup of the service of that name, with the cgroup.procs its child needs to join.
    std::filesystem::path AddService(std::string_view name) const
    {
        const std::filesystem::path cgroup = scratch.Path() / ("task_" + std::string{name});
        std::filesystem::create_directories(cgroup);
        WriteText(cgroup / "cgroup.procs", "");
        return cgroup;
    }

private:
    ScratchDirectory scratch;
    std::filesystem::path previous;
};

// Whether the service's child wrote its pid into the stand-in cgroup.procs, which it does before exec.
bool HasJoined(const tepegoz::Process& process, const std::filesystem::path& cgroup)
{
    return WaitUntil([&process, &cgroup]()
                     { return ReadFile(cgroup / "cgroup.procs") == std::to_string(process.GetPid()); });
}

// How many seconds this test program's wall clock is ahead of the machine's: see clock_gettime below.
std::atomic<long> wallClockOffsetSeconds{0};

// Sets this test program's wall clock ahead by the step, or back with a negative one, while it lives, as setting
// the machine's time would. The steady clock keeps its pace.
class WallClockStep
{
public:
    explicit WallClockStep(std::chrono::seconds step)
    {
        wallClockOffsetSeconds = static_cast<long>(step.count());
    }

    ~WallClockStep()
    {
        wallClockOffsetSeconds = 0;
    }

    WallClockStep(const WallClockStep&) = delete;
    WallClockStep& operator=(const WallClockStep&) = delete;
};

} // namespace

// This definition takes the place of the C library's clock_gettime for the whole test program, the standard
// library's clocks included, so that a test can set the wall clock: CLOCK_REALTIME reads wallClockOffsetSeconds
// ahead. Every other clock is passed through. A test that steps the clock first checks that
// std::chrono::system_clock followed, since without that it would pass without testing anything.
extern "C" int clock_gettime(clockid_t id, timespec* time) noexcept
{
    const int result = static_cast<int>(::syscall(SYS_clock_gettime, id, time));
    if (result == 0 && id == CLOCK_REALTIME)
    {
        time->tv_sec += wallClockOffsetSeconds;
    }

    return result;
}

// ---- Configuration and getters

TEST(ProcessTest, UpdateArgsReturnsOneForCompleteSection)
{
    tepegoz::Process process{};
    EXPECT_EQ(Configure(process, Service("complete", "tpz_steady", "")), 1);
    EXPECT_EQ(process.GetProcessName(), "complete");
}

TEST(ProcessTest, UpdateArgsReturnsMinusOneWithoutProcessName)
{
    tepegoz::Process process{};
    EXPECT_EQ(Configure(process, "ExecutablePath: " + Program("tpz_steady") + "\n"), -1);
    EXPECT_EQ(process.GetProcessName(), "");
}

TEST(ProcessTest, UpdateArgsReturnsMinusOneWithoutExecutablePath)
{
    tepegoz::Process process{};
    EXPECT_EQ(Configure(process, "ProcessName: nopath\n"), -1);
}

TEST(ProcessTest, UpdateArgsKeepsProcessNameWhenItReturnsMinusOneForMissingExecutablePath)
{
    tepegoz::Process process{};
    EXPECT_EQ(Configure(process, "ProcessName: half\nDependsOn: [other]\n"), -1);
    EXPECT_EQ(process.GetProcessName(), "half");
    EXPECT_TRUE(process.GetDependencyList().empty());
}

TEST(ProcessTest, UpdateArgsReturnsMinusTwoForReservedNames)
{
    for (const std::string name : {"pm", "all", "BPM", "PMC", "GUI", "a/b"})
    {
        tepegoz::Process process{};
        EXPECT_EQ(Configure(process, "ProcessName: " + name + "\nExecutablePath: " + Program("tpz_steady") + "\n"), -2)
            << name;
    }
}

TEST(ProcessTest, UpdateArgsReturnsMinusThreeForAKeyInBothSpellings)
{
    tepegoz::Process environment{};
    EXPECT_EQ(Configure(environment, Service("both", "tpz_steady",
                                             "EnvironmentVariables: {A: 1}\nEnviromentVariables: {B: 2}\n")),
              -3);

    tepegoz::Process inherit{};
    EXPECT_EQ(Configure(inherit, Service("both", "tpz_steady",
                                         "InheritParentEnvironment: false\nInheritParentEnviroment: true\n")),
              -3);
}

TEST(ProcessTest, ReservedNameIsReportedBeforeMissingExecutablePath)
{
    tepegoz::Process process{};
    EXPECT_EQ(Configure(process, "ProcessName: pm\n"), -2);
}

TEST(ProcessTest, ReservedNamesAreCaseSensitiveAndIncludeEmptyName)
{
    EXPECT_TRUE(tepegoz::ProcessConfigParams::IsReservedName(""));
    EXPECT_TRUE(tepegoz::ProcessConfigParams::IsReservedName("x/y"));
    EXPECT_TRUE(tepegoz::ProcessConfigParams::IsReservedName("GUI"));
    EXPECT_FALSE(tepegoz::ProcessConfigParams::IsReservedName("PM"));
    EXPECT_FALSE(tepegoz::ProcessConfigParams::IsReservedName("gui"));
    EXPECT_FALSE(tepegoz::ProcessConfigParams::IsReservedName("service"));
}

TEST(ProcessTest, DependencyListReadsSequenceOrSpaceSeparatedText)
{
    tepegoz::Process listed{};
    ASSERT_EQ(Configure(listed, Service("listed", "tpz_steady", "DependsOn: [first, second]\n")), 1);
    EXPECT_EQ(listed.GetDependencyList(), (std::vector<std::string>{"first", "second"}));

    tepegoz::Process text{};
    ASSERT_EQ(Configure(text, Service("text", "tpz_steady", "DependsOn: \"first  second\"\n")), 1);
    EXPECT_EQ(text.GetDependencyList(), (std::vector<std::string>{"first", "second"}));

    tepegoz::Process none{};
    ASSERT_EQ(Configure(none, Service("none", "tpz_steady", "")), 1);
    EXPECT_TRUE(none.GetDependencyList().empty());
}

TEST(ProcessTest, RestartAndStdioDefaultsAreTheDocumentedOnes)
{
    const tepegoz::ProcessConfigParams defaults{};
    EXPECT_EQ(defaults.restartPolicy.maxRestarts, 5);
    EXPECT_FALSE(defaults.stdioPolicy.stdioAppend);
    EXPECT_EQ(defaults.stdioPolicy.stdioFilePath, std::filesystem::path{"/tmp/output.txt"});
}

TEST(ProcessTest, DefaultsInTheHeadersMatchTheConfigReader)
{
    config_section::ConfigValue value{};
    std::string error{};
    ASSERT_EQ(config_section::ParseConfigText("ProcessName: defaults\nExecutablePath: /bin/true\n", value, error),
              config_section::LoadCode::Ok)
        << error;
    config_section::ConfigSection section{value, "test service"};
    tepegoz::ProcessConfigParams read{};
    ASSERT_EQ(read.UpdateArgs(section), 1);
    const tepegoz::ProcessConfigParams defaults{};

    EXPECT_EQ(read.args, defaults.args);
    EXPECT_EQ(read.cwd, defaults.cwd);
    EXPECT_EQ(read.inheritParentEnvironment, defaults.inheritParentEnvironment);
    EXPECT_EQ(read.useDisplay, defaults.useDisplay);
    EXPECT_EQ(read.display, defaults.display);
    EXPECT_EQ(read.xAuthority, defaults.xAuthority);
    EXPECT_EQ(read.environmentVariables, defaults.environmentVariables);
    EXPECT_EQ(read.forceKillTimeoutMs, defaults.forceKillTimeoutMs);
    EXPECT_EQ(read.createProcessGroup, defaults.createProcessGroup);
    EXPECT_EQ(read.dependsOn, defaults.dependsOn);
    EXPECT_EQ(read.workMode, defaults.workMode);

    EXPECT_EQ(read.stdioPolicy.stdioMode, defaults.stdioPolicy.stdioMode);
    EXPECT_EQ(read.stdioPolicy.stdioFilePath, defaults.stdioPolicy.stdioFilePath);
    EXPECT_EQ(read.stdioPolicy.stdioAppend, defaults.stdioPolicy.stdioAppend);

    EXPECT_EQ(read.restartPolicy.restartMode, defaults.restartPolicy.restartMode);
    EXPECT_EQ(read.restartPolicy.maxRestarts, defaults.restartPolicy.maxRestarts);
    EXPECT_DOUBLE_EQ(read.restartPolicy.backoffMultiplier, defaults.restartPolicy.backoffMultiplier);
    EXPECT_EQ(read.restartPolicy.restartDelayMs, defaults.restartPolicy.restartDelayMs);
    EXPECT_EQ(read.restartPolicy.restartResetMs, defaults.restartPolicy.restartResetMs);

    EXPECT_EQ(read.healthPolicy.checkHealth, defaults.healthPolicy.checkHealth);
    EXPECT_EQ(read.healthPolicy.gracePeriodMs, defaults.healthPolicy.gracePeriodMs);
    EXPECT_EQ(read.healthPolicy.heartbeatPeriodMs, defaults.healthPolicy.heartbeatPeriodMs);
    EXPECT_EQ(read.healthPolicy.missedHeartbeatThreshold, defaults.healthPolicy.missedHeartbeatThreshold);
}

TEST(ProcessTest, DefaultConstructedProcessHasNoPidAndNeedsStart)
{
    tepegoz::Process process{};
    EXPECT_EQ(process.GetPid(), -1);
    EXPECT_FALSE(process.HasPid());
    EXPECT_TRUE(process.NeedsStart());
    EXPECT_FALSE(process.NeedsRestart());
    EXPECT_EQ(process.GetRuntimeState(), tepegoz::RuntimeState::Unknown);
    EXPECT_FALSE(process.IsRunning());
    EXPECT_FALSE(process.IsStarting());
    EXPECT_FALSE(process.IsStopping());
    EXPECT_FALSE(process.IsRestartRequested());
    EXPECT_FALSE(process.IsGracePeriod());
    EXPECT_TRUE(process.IsWorkingManually());
    EXPECT_EQ(process.ReapExit(), 0);
    EXPECT_EQ(process.GetRuntimeStats().restartCount, 0);
    EXPECT_FALSE(process.IsMember(1, 1, "/sys/fs/cgroup/task_"));
}

TEST(ProcessTest, WorkModeDefaultsToManualAndFollowsConfigAndSetWorkMode)
{
    tepegoz::Process unset{};
    ASSERT_EQ(Configure(unset, Service("unset", "tpz_steady", "")), 1);
    EXPECT_TRUE(unset.IsWorkingManually());
    unset.SetWorkMode(tepegoz::WorkMode::Auto);
    EXPECT_FALSE(unset.IsWorkingManually());
    unset.SetWorkMode(tepegoz::WorkMode::Manual);
    EXPECT_TRUE(unset.IsWorkingManually());

    tepegoz::Process automatic{};
    ASSERT_EQ(Configure(automatic, Service("automatic", "tpz_steady", "WorkMode: auto\n")), 1);
    EXPECT_FALSE(automatic.IsWorkingManually());
}

TEST(ProcessTest, CheckConfigAcceptsValidService)
{
    const ScratchDirectory scratch{};
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("valid", "tpz_steady",
                                         "Cwd: " + scratch.Path().string() + "\n" + scratch.FileSettings("out.txt"))),
              1);
    std::vector<std::string> errors{};
    process.CheckConfig(errors);
    EXPECT_TRUE(errors.empty());
}

TEST(ProcessTest, CheckConfigReportsEachProblemOfMissingExecutableOnce)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, "ProcessName: svc\nExecutablePath: /nonexistent/tpz_missing\n"), 1);
    std::vector<std::string> errors{"earlier"};
    process.CheckConfig(errors);
    EXPECT_EQ(errors, (std::vector<std::string>{
                          "earlier",
                          "svc ExecutablePath does not exists",
                          "svc ExecutablePath is not a regular file",
                          "svc ExecutablePath access denied",
                      }));
}

TEST(ProcessTest, CheckConfigReportsDirectoryAsExecutable)
{
    const ScratchDirectory scratch{};
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, "ProcessName: svc\nExecutablePath: " + scratch.Path().string() + "\n"), 1);
    std::vector<std::string> errors{};
    process.CheckConfig(errors);
    EXPECT_EQ(errors, (std::vector<std::string>{
                          "svc ExecutablePath is a directory",
                          "svc ExecutablePath is not a regular file",
                      }));
}

TEST(ProcessTest, CheckConfigAcceptsSymlinkToExecutable)
{
    const ScratchDirectory scratch{};
    const std::filesystem::path link = scratch.Path() / "steady_link";
    std::filesystem::create_symlink(Program("tpz_steady"), link);
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, "ProcessName: svc\nExecutablePath: " + link.string() + "\n"), 1);
    std::vector<std::string> errors{};
    process.CheckConfig(errors);
    EXPECT_TRUE(errors.empty());
}

TEST(ProcessTest, CheckConfigReportsDanglingSymlinkAsMissingExecutable)
{
    const ScratchDirectory scratch{};
    const std::filesystem::path link = scratch.Path() / "dangling_link";
    std::filesystem::create_symlink(scratch.Path() / "nowhere", link);
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, "ProcessName: svc\nExecutablePath: " + link.string() + "\n"), 1);
    std::vector<std::string> errors{};
    process.CheckConfig(errors);
    EXPECT_EQ(errors, (std::vector<std::string>{
                          "svc ExecutablePath does not exists",
                          "svc ExecutablePath is not a regular file",
                          "svc ExecutablePath access denied",
                      }));
}

TEST(ProcessTest, CheckConfigReportsMissingCwdTwice)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("svc", "tpz_steady", "Cwd: /nonexistent/tpz\n")), 1);
    std::vector<std::string> errors{};
    process.CheckConfig(errors);
    EXPECT_EQ(errors, (std::vector<std::string>{"svc Cwd does not exists", "svc Cwd is not a directory"}));
}

TEST(ProcessTest, CheckConfigReportsMissingStdioFileDirectory)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("svc", "tpz_steady", "StdioMode: File\nStdioFilePath: /nonexistent/tpz/out.txt\n")),
              1);
    std::vector<std::string> errors{};
    process.CheckConfig(errors);
    EXPECT_EQ(errors, (std::vector<std::string>{"svc StdioFilePath directory does not exists"}));
}

TEST(ProcessTest, CheckConfigReportsEmptyStdioFilePath)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("svc", "tpz_steady", "StdioMode: File\nStdioFilePath: \"\"\n")), 1);
    std::vector<std::string> errors{};
    process.CheckConfig(errors);
    EXPECT_EQ(errors, (std::vector<std::string>{"svc StdioFilePath is empty logging will fail"}));
}

TEST(ProcessTest, CheckConfigIgnoresStdioFilePathOutsideFileMode)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("svc", "tpz_steady", "StdioMode: Pipe\nStdioFilePath: /nonexistent/tpz/out.txt\n")),
              1);
    std::vector<std::string> errors{};
    process.CheckConfig(errors);
    EXPECT_TRUE(errors.empty());
}

TEST(ProcessTest, HealthReportTruncatesNameAndCarriesPidAndState)
{
    const std::string longName(70, 'n');
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service(longName, "tpz_steady", "")), 1);

    const tepegoz::DetailedHealthReport idle = process.GetHealthReport();
    EXPECT_EQ(std::string{idle.processName}, std::string(63, 'n'));
    EXPECT_EQ(idle.pid, -1);
    EXPECT_EQ(idle.runtimeHealth.state, tepegoz::RuntimeState::Unknown);
    EXPECT_EQ(idle.gpuUsage.state, tepegoz::GpuUsageState::NotSampled);

    process.StartProcess();
    ASSERT_TRUE(process.HasPid());
    const tepegoz::DetailedHealthReport running = process.GetHealthReport();
    EXPECT_EQ(running.pid, process.GetPid());
    EXPECT_EQ(running.runtimeHealth.state, tepegoz::RuntimeState::Running);
    EXPECT_EQ(running.runtimeHealth.restartCount, 0);
    EXPECT_EQ(running.runtimeHealth.missedBeats, 0);
    EXPECT_EQ(running.runtimeHealth.startTime, process.GetRuntimeStats().startTime);
    EXPECT_GE(running.snapshotTime, running.runtimeHealth.startTime);
}

// ---- Start and exit

TEST(ProcessTest, StartGivesRunningProcessWithPidInItsOwnProcessGroup)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("start", "tpz_steady", "")), 1);
    process.StartProcess();
    ASSERT_TRUE(process.HasPid());
    const pid_t pid = process.GetPid();
    EXPECT_GT(pid, 0);
    EXPECT_TRUE(IsAlive(pid));
    EXPECT_EQ(::getpgid(pid), pid);
    EXPECT_TRUE(process.IsRunning());
    EXPECT_FALSE(process.IsStarting());
    EXPECT_EQ(process.GetRuntimeState(), tepegoz::RuntimeState::Running);
    EXPECT_FALSE(process.NeedsStart());
    EXPECT_FALSE(process.NeedsRestart());
    EXPECT_EQ(process.ReapExit(), -1);
    EXPECT_NE(process.GetRuntimeStats().startTime, std::chrono::system_clock::time_point{});
    EXPECT_GE(process.GetRuntimeStats().lastSeen, process.GetRuntimeStats().startTime);

    EXPECT_TRUE(process.IsMember(pid, -1, {}));
    EXPECT_TRUE(process.IsMember(pid + 100000, pid, {}));
    EXPECT_FALSE(process.IsMember(pid + 100000, pid + 100000, {}));
    EXPECT_FALSE(process.IsMember(pid + 100000, 0, {}));
}

TEST(ProcessTest, StartAndAutoRestartWhileRunningKeepTheSamePid)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("twice", "tpz_steady", "")), 1);
    process.StartProcess();
    const pid_t pid = process.GetPid();
    process.StartProcess();
    EXPECT_EQ(process.GetPid(), pid);
    process.AutoRestart();
    EXPECT_EQ(process.GetPid(), pid);
    EXPECT_EQ(process.GetRuntimeStats().restartCount, 0);
}

TEST(ProcessTest, CleanExitIsReapedAndShownAsStopped)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("clean", "tpz_exit", "Args: [--code, 0, --after-ms, 0]\nRestartDelayMs: 0\n")), 1);
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
    EXPECT_EQ(process.GetRuntimeState(), tepegoz::RuntimeState::Stopped);
    EXPECT_FALSE(process.IsRunning());
    EXPECT_EQ(process.ReapExit(), 0);
    EXPECT_FALSE(process.NeedsStart());
    EXPECT_FALSE(process.NeedsRestart());
    EXPECT_FALSE(process.IsMember(pid, pid, {}));
    EXPECT_FALSE(IsAlive(pid));
}

TEST(ProcessTest, RestartCountCountsEveryLaunchAfterTheFirst)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("counted", "tpz_exit", "Args: [--code, 0, --after-ms, 0]\n")), 1);
    process.StartProcess();
    EXPECT_EQ(process.GetRuntimeStats().restartCount, 0);
    ASSERT_TRUE(WaitForExit(process));
    process.StartProcess();
    EXPECT_EQ(process.GetRuntimeStats().restartCount, 1);
    ASSERT_TRUE(WaitForExit(process));
    process.AutoRestart();
    EXPECT_EQ(process.GetRuntimeStats().restartCount, 2);
    EXPECT_EQ(process.GetHealthReport().runtimeHealth.restartCount, 2);
}

TEST(ProcessTest, MissingExecutableStartsAsRunningThenExitsAsFailure)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, "ProcessName: missing\nExecutablePath: /nonexistent/tpz_missing\nStdioMode: NullDevice\n"
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
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("nofile", "tpz_steady",
                                         "StdioMode: File\nStdioFilePath: /nonexistent/tpz/out.txt\nRestartDelayMs: 0\n")),
              1);
    process.StartProcess();
    EXPECT_TRUE(process.HasPid());
    ASSERT_TRUE(WaitForExit(process));
    EXPECT_TRUE(process.NeedsRestart());
}

// ---- Restart policy

TEST(ProcessTest, OnFailureRestartsAfterNonZeroExitCode)
{
    EXPECT_TRUE(RestartDueAfterExit("OnFailure", "tpz_exit", "--code, 3, --after-ms, 0"));
}

TEST(ProcessTest, OnFailureDoesNotRestartAfterExitCodeZero)
{
    EXPECT_FALSE(RestartDueAfterExit("OnFailure", "tpz_exit", "--code, 0, --after-ms, 0"));
}

TEST(ProcessTest, OnFailureRestartsAfterCrashSignals)
{
    for (const std::string signal : {"SEGV", "ABRT", "KILL", "USR1"})
    {
        EXPECT_TRUE(RestartDueAfterExit("OnFailure", "tpz_crash", "--signal, " + signal + ", --after-ms, 0")) << signal;
    }
}

TEST(ProcessTest, OnFailureTreatsTermHupIntAndPipeSignalsAsCleanStop)
{
    for (const std::string signal : {"TERM", "HUP", "INT", "PIPE"})
    {
        EXPECT_FALSE(RestartDueAfterExit("OnFailure", "tpz_crash", "--signal, " + signal + ", --after-ms, 0")) << signal;
    }
}

TEST(ProcessTest, AlwaysRestartsAfterCleanExit)
{
    EXPECT_TRUE(RestartDueAfterExit("Always", "tpz_exit", "--code, 0, --after-ms, 0"));
}

TEST(ProcessTest, NeverDoesNotRestartAfterFailure)
{
    EXPECT_FALSE(RestartDueAfterExit("Never", "tpz_exit", "--code, 3, --after-ms, 0"));
}

TEST(ProcessTest, AlwaysRestartsAfterStop)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("always", "tpz_steady", "RestartMode: Always\nRestartDelayMs: 0\n")), 1);
    process.StartProcess();
    ASSERT_TRUE(StopAndWait(process));
    EXPECT_FALSE(process.HasPid());
    EXPECT_TRUE(process.NeedsRestart());
}

TEST(ProcessTest, UnlessStoppedRestartsAfterCleanExit)
{
    EXPECT_TRUE(RestartDueAfterExit("UnlessStopped", "tpz_exit", "--code, 0, --after-ms, 0"));
}

TEST(ProcessTest, UnlessStoppedKeepsStoppedServiceStoppedUntilTheNextStart)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("unless", "tpz_exit",
                                         "Args: [--code, 0, --after-ms, 300]\nRestartMode: UnlessStopped\n"
                                         "RestartDelayMs: 0\n")),
              1);
    process.StartProcess();
    ASSERT_TRUE(StopAndWait(process));
    EXPECT_FALSE(process.HasPid());
    EXPECT_FALSE(process.NeedsRestart());

    process.StartProcess();
    ASSERT_TRUE(WaitForExit(process));
    EXPECT_TRUE(process.NeedsRestart());
}

TEST(ProcessTest, UnlessStoppedStopWhileARestartIsDueCancelsIt)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("unless", "tpz_exit",
                                         "Args: [--code, 3, --after-ms, 0]\nRestartMode: UnlessStopped\n"
                                         "RestartDelayMs: 0\n")),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForExit(process));
    ASSERT_TRUE(process.NeedsRestart());

    process.Stop();
    EXPECT_FALSE(process.IsStopping());
    EXPECT_FALSE(process.NeedsRestart());
}

TEST(ProcessTest, OnFailureDoesNotRestartExitReapedElsewhere)
{
    EXPECT_FALSE(RestartDueAfterCleanExitReapedElsewhere("OnFailure"));
}

TEST(ProcessTest, AlwaysRestartsExitReapedElsewhere)
{
    EXPECT_TRUE(RestartDueAfterCleanExitReapedElsewhere("Always"));
}

TEST(ProcessTest, RestartDelayGrowsByBackoffMultiplierOnEachRestartInARow)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("backoff", "tpz_exit",
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

    EXPECT_EQ(process.GetRuntimeStats().restartCount, 3);
}

TEST(ProcessTest, RestartLimitStopsAutomaticRestarts)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("limited", "tpz_exit",
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
    EXPECT_EQ(process.GetRuntimeStats().restartCount, 2);
}

TEST(ProcessTest, ManualStartRearmsRestartLimit)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("rearmed", "tpz_exit",
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
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("zero", "tpz_exit",
                                         "Args: [--code, 3, --after-ms, 0]\nRestartDelayMs: 0\nMaxRestarts: 0\n")),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForExit(process));
    EXPECT_FALSE(process.NeedsRestart());
}

TEST(ProcessTest, RunAsLongAsRestartResetMsStartsANewRestartSeries)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("reset", "tpz_exit",
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

    EXPECT_EQ(process.GetRuntimeStats().restartCount, 3);
}

// ---- Stopping

TEST(ProcessTest, StopWithoutPidDoesNothing)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("idle", "tpz_steady", "")), 1);
    process.Stop();
    EXPECT_FALSE(process.IsStopping());
    process.RequestStopAndEscalate();
    EXPECT_FALSE(process.IsStopping());
    process.AdvanceStop();
    EXPECT_EQ(process.GetRuntimeState(), tepegoz::RuntimeState::Unknown);
}

TEST(ProcessTest, StopEndsServiceThatHonoursSigtermWithoutForceKill)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("graceful", "tpz_steady",
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
    EXPECT_EQ(process.GetRuntimeState(), tepegoz::RuntimeState::Stopped);
    EXPECT_FALSE(process.NeedsRestart());
}

TEST(ProcessTest, StopKillsServiceAndChildIgnoringSigtermAfterForceKillTimeout)
{
    const ScratchDirectory scratch{};
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("stubborn", "tpz_stubborn",
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
    // Ended by SIGKILL, but the stop asked for that: OnFailure does not count it as a failure.
    EXPECT_FALSE(process.NeedsRestart());
}

TEST(ProcessTest, ForceKillTimeoutBelowOneSecondIsRaisedToOneSecond)
{
    const ScratchDirectory scratch{};
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("quick", "tpz_stubborn",
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
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("forker", "tpz_forker",
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
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("escape", "tpz_forker",
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

TEST(ProcessTest, ChildrenOfLeaderThatExitsOnItsOwnAreStoppedBeforeARestart)
{
    const ScratchDirectory scratch{};
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("orphans", "tpz_forker",
                                         "Args: [--children, 1, --crash-parent-after-ms, 200, --period-ms, 50, "
                                         "--child-lifetime-ms, 5000]\nRestartDelayMs: 0\n" +
                                             scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    const pid_t leader = process.GetPid();
    ASSERT_TRUE(WaitForExit(process));
    const pid_t child = PidAfter(ReadFile(scratch.Path() / "out.txt"), "started child ");
    ASSERT_GT(child, 0);

    EXPECT_TRUE(process.IsStopping());
    EXPECT_EQ(process.GetRuntimeState(), tepegoz::RuntimeState::Stopped);
    EXPECT_FALSE(process.NeedsRestart());
    const bool stopped = WaitForStop(process);
    const bool childAlive = IsAlive(child);
    ::kill(-leader, SIGKILL);
    ASSERT_TRUE(stopped);
    EXPECT_FALSE(childAlive);
    // The leader crashed with SIGSEGV, which OnFailure restarts once the child is gone.
    EXPECT_TRUE(process.NeedsRestart());
}

TEST(ProcessTest, StartWhileLeftoversAreStoppedWaitsForThem)
{
    const ScratchDirectory scratch{};
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("waits", "tpz_forker",
                                         "Args: [--children, 1, --crash-parent-after-ms, 200, --period-ms, 50, "
                                         "--child-lifetime-ms, 5000]\n" +
                                             scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    const pid_t leader = process.GetPid();
    ASSERT_TRUE(WaitForExit(process));
    ASSERT_TRUE(process.IsStopping());

    process.StartProcess();
    EXPECT_FALSE(process.HasPid());
    EXPECT_TRUE(process.IsRestartRequested());
    process.AutoRestart();
    EXPECT_FALSE(process.HasPid());

    const bool stopped = WaitForStop(process);
    ::kill(-leader, SIGKILL);
    ASSERT_TRUE(stopped);
    EXPECT_TRUE(process.IsRestartRequested());
    process.StartProcess();
    EXPECT_TRUE(process.HasPid());
    EXPECT_FALSE(process.IsRestartRequested());
}

TEST(ProcessTest, RestartRequestSurvivesTheStopUntilTheNextStart)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("restart", "tpz_steady", "")), 1);
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
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("cancel", "tpz_steady", "")), 1);
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
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("blocking", "tpz_steady", "")), 1);
    process.StartProcess();
    const pid_t pid = process.GetPid();
    process.RequestStopAndEscalate(std::chrono::milliseconds{10});
    EXPECT_FALSE(process.IsStopping());
    EXPECT_FALSE(process.HasPid());
    EXPECT_FALSE(IsAlive(pid));
    EXPECT_EQ(process.GetRuntimeState(), tepegoz::RuntimeState::Stopped);
}

TEST(ProcessTest, DestructorStopsTheRunningChild)
{
    pid_t pid = -1;
    {
        tepegoz::Process process{};
        ASSERT_EQ(Configure(process, Service("scoped", "tpz_steady", "")), 1);
        process.StartProcess();
        pid = process.GetPid();
        ASSERT_TRUE(IsAlive(pid));
    }

    EXPECT_FALSE(IsAlive(pid));
}

TEST(ProcessTest, MoveConstructionHandsTheChildOver)
{
    tepegoz::Process source{};
    ASSERT_EQ(Configure(source, Service("moved", "tpz_steady", "")), 1);
    source.StartProcess();
    const pid_t pid = source.GetPid();

    tepegoz::Process target{std::move(source)};
    EXPECT_FALSE(source.HasPid());
    EXPECT_EQ(source.GetPid(), -1);
    EXPECT_EQ(target.GetPid(), pid);
    EXPECT_EQ(target.GetProcessName(), "moved");
    EXPECT_TRUE(target.IsRunning());
    EXPECT_TRUE(IsAlive(pid));
}

TEST(ProcessTest, MoveAssignmentStopsTheChildItReplaces)
{
    tepegoz::Process source{};
    ASSERT_EQ(Configure(source, Service("incoming", "tpz_steady", "")), 1);
    source.StartProcess();
    const pid_t incoming = source.GetPid();

    tepegoz::Process target{};
    ASSERT_EQ(Configure(target, Service("replaced", "tpz_steady", "")), 1);
    target.StartProcess();
    const pid_t replaced = target.GetPid();

    target = std::move(source);
    EXPECT_FALSE(IsAlive(replaced));
    EXPECT_TRUE(IsAlive(incoming));
    EXPECT_EQ(target.GetPid(), incoming);
    EXPECT_EQ(target.GetProcessName(), "incoming");
    EXPECT_FALSE(source.HasPid());
}

TEST(ProcessTest, MoveConstructionHandsOverTheStopOfLeftovers)
{
    const ScratchDirectory scratch{};
    tepegoz::Process source{};
    ASSERT_EQ(Configure(source, Service("handed", "tpz_forker",
                                        "Args: [--children, 1, --crash-parent-after-ms, 200, --period-ms, 50, "
                                        "--child-lifetime-ms, 5000]\n" +
                                            scratch.FileSettings("out.txt"))),
              1);
    source.StartProcess();
    const pid_t leader = source.GetPid();
    ASSERT_TRUE(WaitForExit(source));
    ASSERT_TRUE(source.IsStopping());

    tepegoz::Process target{std::move(source)};
    EXPECT_FALSE(source.IsStopping());
    EXPECT_TRUE(target.IsStopping());
    const bool stopped = WaitForStop(target);
    ::kill(-leader, SIGKILL);
    EXPECT_TRUE(stopped);
}

// ---- What reaches the child

TEST(ProcessTest, ArgsListKeepsItemWithSpacesInOnePiece)
{
    const ScratchDirectory scratch{};
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("args", "tpz_env", "Args: [--show, HOME, two words, plain]\n" +
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
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process,
                        Service("args", "tpz_env", "Args: --show HOME two words\n" + scratch.FileSettings("out.txt"))),
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
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("override", "tpz_env", "Args: [--show, HOME, extra]\n" +
                                                                   scratch.FileSettings("out.txt"))),
              1);
    process.OverrideStartupArgs({"--show", "TPZ_OVERRIDE"});
    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "env TPZ_OVERRIDE"));
    const std::string text = ReadFile(scratch.Path() / "out.txt");
    EXPECT_NE(text.find("argc 3"), std::string::npos) << text;
    EXPECT_NE(text.find("argv[2]='TPZ_OVERRIDE'"), std::string::npos) << text;
}

TEST(ProcessTest, EnvironmentVariablesAreAddedToTheInheritedEnvironment)
{
    const ScratchDirectory scratch{};
    ASSERT_EQ(::setenv("TPZ_INHERITED", "yes", 1), 0);
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("env", "tpz_env",
                                         "Args: [--show, TPZ_COLOR, --show, TPZ_EMPTY, --show, TPZ_INHERITED]\n"
                                         "EnvironmentVariables: {TPZ_COLOR: blue, TPZ_EMPTY: \"\"}\n" +
                                             scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "env TPZ_INHERITED"));
    ::unsetenv("TPZ_INHERITED");
    const std::string text = ReadFile(scratch.Path() / "out.txt");
    EXPECT_NE(text.find("env TPZ_COLOR='blue'"), std::string::npos) << text;
    EXPECT_NE(text.find("env TPZ_EMPTY=''"), std::string::npos) << text;
    EXPECT_NE(text.find("env TPZ_INHERITED='yes'"), std::string::npos) << text;
}

TEST(ProcessTest, ClearedEnvironmentHoldsOnlyTheConfiguredVariables)
{
    const ScratchDirectory scratch{};
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("cleared", "tpz_env",
                                         "Args: [--all-env]\nInheritParentEnvironment: false\n"
                                         "EnvironmentVariables: TPZ_ONLY=this\n" +
                                             scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "environ "));
    const std::string text = ReadFile(scratch.Path() / "out.txt");
    EXPECT_NE(text.find("environment has 1 variables"), std::string::npos) << text;
    EXPECT_NE(text.find("environ TPZ_ONLY=this"), std::string::npos) << text;
    EXPECT_NE(text.find("env PATH unset"), std::string::npos) << text;
}

TEST(ProcessTest, OldSpellingsOfTheEnvironmentKeysStillWork)
{
    const ScratchDirectory scratch{};
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("old", "tpz_env",
                                         "Args: [--all-env]\nInheritParentEnviroment: false\n"
                                         "EnviromentVariables: TPZ_ONLY=this\n" +
                                             scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "environ "));
    const std::string text = ReadFile(scratch.Path() / "out.txt");
    EXPECT_NE(text.find("environment has 1 variables"), std::string::npos) << text;
    EXPECT_NE(text.find("environ TPZ_ONLY=this"), std::string::npos) << text;
}

TEST(ProcessTest, UseDisplaySetsDisplayAndXauthority)
{
    const ScratchDirectory scratch{};
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("display", "tpz_env",
                                         "UseDisplay: true\nDisplay: \":7\"\nXAuthority: /tmp/tpz.Xauthority\n" +
                                             scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "env JOURNAL_STREAM"));
    const std::string text = ReadFile(scratch.Path() / "out.txt");
    EXPECT_NE(text.find("env DISPLAY=':7'"), std::string::npos) << text;
    EXPECT_NE(text.find("env XAUTHORITY='/tmp/tpz.Xauthority'"), std::string::npos) << text;
}

TEST(ProcessTest, CwdBecomesTheChildWorkingDirectory)
{
    const ScratchDirectory scratch{};
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("cwd", "tpz_env",
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
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("nocwd", "tpz_env", "Cwd: /nonexistent/tpz\n" + scratch.FileSettings("out.txt"))),
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
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("truncated", "tpz_env", scratch.FileSettings("out.txt"))), 1);
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
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("appended", "tpz_env", "StdioAppend: true\n" + scratch.FileSettings("out.txt"))),
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

    tepegoz::Process process{};
    const int configured = Configure(process, Service("piped", "tpz_env", "Args: [--show, TPZ_PIPE]\nStdioMode: Pipe\n"));
    process.StartProcess();
    const bool logged = WaitUntil(
        [&process, &logPath]()
        {
            process.DrainStdioPipe();
            return ReadFile(logPath).find("env TPZ_PIPE unset") != std::string::npos;
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
            EXPECT_NE(line.find("tpz_env["), std::string::npos) << line;
        }
    }

    EXPECT_TRUE(found) << text;
}

// ---- Health and heartbeats, driven directly without the broker

TEST(ProcessTest, HealthCheckStartsInStartingStateUntilFirstHeartbeat)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("health", "tpz_steady", "CheckHealth: true\nGracePeriodMs: 2000\n")), 1);
    process.StartProcess();
    EXPECT_TRUE(process.IsStarting());
    EXPECT_FALSE(process.IsRunning());
    EXPECT_EQ(process.GetRuntimeState(), tepegoz::RuntimeState::Starting);

    process.HeartbeatReceived();
    EXPECT_TRUE(process.IsRunning());
    EXPECT_FALSE(process.IsStarting());
    EXPECT_EQ(process.GetRuntimeStats().missedBeats, 0);
}

TEST(ProcessTest, HeartbeatWithoutHealthCheckChangesNothing)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("nohealth", "tpz_steady", "GracePeriodMs: 0\nHeartbeatPeriodMs: 1\n")), 1);
    process.StartProcess();
    const tepegoz::RuntimeHealth before = process.GetRuntimeStats();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    process.HeartbeatReceived();
    process.CheckHeartbeat();
    EXPECT_EQ(process.GetRuntimeState(), tepegoz::RuntimeState::Running);
    EXPECT_EQ(process.GetRuntimeStats().lastSeen, before.lastSeen);
    EXPECT_EQ(process.GetRuntimeStats().missedBeats, 0);
}

TEST(ProcessTest, GracePeriodEndsAfterGracePeriodMs)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("grace", "tpz_steady", "GracePeriodMs: 100\n")), 1);
    const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    process.StartProcess();
    EXPECT_TRUE(process.IsGracePeriod());
    ASSERT_TRUE(WaitUntil([&process]() { return !process.IsGracePeriod(); }));
    EXPECT_GE(std::chrono::steady_clock::now() - begin, std::chrono::milliseconds{100});
}

TEST(ProcessTest, ZeroGracePeriodMeansNoGracePeriod)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("nograce", "tpz_steady", "GracePeriodMs: 0\n")), 1);
    process.StartProcess();
    EXPECT_FALSE(process.IsGracePeriod());
}

TEST(ProcessTest, CheckHeartbeatDoesNothingDuringGracePeriod)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("graceful", "tpz_steady",
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
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("silent", "tpz_steady",
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
            return process.GetRuntimeState() == tepegoz::RuntimeState::Unhealthy;
        }));
    EXPECT_GE(Clock::now() - begin, std::chrono::milliseconds{60});
    EXPECT_GE(process.GetRuntimeStats().missedBeats, 3);
    EXPECT_FALSE(process.IsRunning());
    EXPECT_FALSE(process.IsStarting());
    EXPECT_TRUE(process.HasPid());
}

TEST(ProcessTest, HeartbeatBringsUnhealthyServiceBackToRunning)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("flaky", "tpz_steady",
                                         "CheckHealth: true\nGracePeriodMs: 0\nHeartbeatPeriodMs: 10\n"
                                         "MissedHeartbeatThreshold: 1\n")),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitUntil(
        [&process]()
        {
            process.CheckHeartbeat();
            return process.GetRuntimeState() == tepegoz::RuntimeState::Unhealthy;
        }));
    const std::chrono::system_clock::time_point lastSeenBefore = process.GetRuntimeStats().lastSeen;

    process.HeartbeatReceived();
    EXPECT_TRUE(process.IsRunning());
    EXPECT_EQ(process.GetRuntimeStats().missedBeats, 0);
    EXPECT_GT(process.GetRuntimeStats().lastSeen, lastSeenBefore);
    EXPECT_FALSE(process.NeedsRestart());
}

TEST(ProcessTest, UnhealthyServiceNeedsRestartAndItsStopCountsAsFailureDespiteCleanExit)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("unresponsive", "tpz_steady",
                                         "CheckHealth: true\nGracePeriodMs: 0\nHeartbeatPeriodMs: 10\n"
                                         "MissedHeartbeatThreshold: 2\nRestartDelayMs: 0\n")),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitUntil(
        [&process]()
        {
            process.CheckHeartbeat();
            return process.GetRuntimeState() == tepegoz::RuntimeState::Unhealthy;
        }));
    EXPECT_TRUE(process.NeedsRestart());

    process.StopUnresponsive();
    EXPECT_TRUE(process.IsStopping());
    EXPECT_FALSE(process.IsRestartRequested());
    ASSERT_TRUE(WaitForStop(process));
    EXPECT_EQ(process.GetRuntimeState(), tepegoz::RuntimeState::Stopped);
    EXPECT_TRUE(process.NeedsRestart());

    process.AutoRestart();
    EXPECT_TRUE(process.HasPid());
    EXPECT_TRUE(process.IsStarting());
    EXPECT_EQ(process.GetRuntimeStats().missedBeats, 0);
}

TEST(ProcessTest, NeverModeDoesNotRestartUnhealthyService)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("ignored", "tpz_steady",
                                         "CheckHealth: true\nGracePeriodMs: 0\nHeartbeatPeriodMs: 10\n"
                                         "MissedHeartbeatThreshold: 1\nRestartMode: Never\n")),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitUntil(
        [&process]()
        {
            process.CheckHeartbeat();
            return process.GetRuntimeState() == tepegoz::RuntimeState::Unhealthy;
        }));
    EXPECT_FALSE(process.NeedsRestart());
}

TEST(ProcessTest, SuspendedHealthCheckIgnoresHeartbeatsUntilNextStart)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("suspended", "tpz_steady",
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
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("late", "tpz_steady",
                                         "CheckHealth: true\nGracePeriodMs: 0\nHeartbeatPeriodMs: 1\n"
                                         "MissedHeartbeatThreshold: 1\n")),
              1);
    process.StartProcess();
    ASSERT_TRUE(StopAndWait(process));
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    process.HeartbeatReceived();
    EXPECT_EQ(process.GetRuntimeState(), tepegoz::RuntimeState::Stopped);
    process.CheckHeartbeat();
    EXPECT_EQ(process.GetRuntimeState(), tepegoz::RuntimeState::Stopped);
    EXPECT_EQ(process.GetRuntimeStats().missedBeats, 0);
}

// ---- Without cgroups (WSL1, containers, no systemd delegation)

TEST(ProcessTest, WithoutCgroupsUsageIsZeroAndCgroupMembershipIsIgnored)
{
    if (CgroupsAvailable())
    {
        GTEST_SKIP() << "cgroups are available here; this pins the path without them";
    }

    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("nocgroup", "tpz_steady", "")), 1);
    process.StartProcess();
    ASSERT_TRUE(process.HasPid());
    EXPECT_TRUE(process.IsRunning());

    const std::filesystem::path own =
        tepegoz::ProcessGroupBuilder::GetInstance().GetParentGroupPath() / "task_nocgroup";
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
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("relative", "tpz_steady", "")), 1);
    EXPECT_EQ(process.GetMemoryUsageInBytes(), 4242u);
    EXPECT_EQ(process.GetCpuUsageInUsec(), 777u);
}

// ---- Killing what is left in a cgroup. A folder stands in for the cgroup, so its files are the test's own.

TEST(ProcessTest, ReadGroupProcessesLeavesOutLinesThatAreNoPid)
{
    const ScratchDirectory cgroup{};
    WriteText(cgroup.Path() / "cgroup.procs", "12\n0\n-7\nabc\n\n 5\n34x\n56\n");
    const std::optional<std::vector<pid_t>> processes =
        tepegoz::ProcessGroupBuilder::ReadGroupProcesses(cgroup.Path());
    ASSERT_TRUE(processes.has_value());
    EXPECT_EQ(*processes, (std::vector<pid_t>{12, 56}));
}

TEST(ProcessTest, ReadGroupProcessesWithoutCgroupProcsReturnsNullopt)
{
    const ScratchDirectory cgroup{};
    EXPECT_FALSE(tepegoz::ProcessGroupBuilder::ReadGroupProcesses(cgroup.Path()).has_value());
}

TEST(ProcessTest, KillGroupLeavesTheKillingToCgroupKillWhenTheFileExists)
{
    const ScratchDirectory cgroup{};
    WriteText(cgroup.Path() / "cgroup.kill", "");
    Bystander bystander{};
    ASSERT_GT(bystander.Pid(), 0);
    ListMembers(cgroup.Path(), {bystander.Pid()});

    EXPECT_EQ(tepegoz::ProcessGroupBuilder::KillGroup(cgroup.Path()), tepegoz::KillCode::Ok);
    EXPECT_EQ(ReadFile(cgroup.Path() / "cgroup.kill"), "1");
    // No kernel acts on this file, so a bystander that lives shows that nothing else signalled it.
    EXPECT_FALSE(bystander.WasKilled(unharmed_limit));
}

TEST(ProcessTest, KillGroupWithoutCgroupKillKillsTheListedProcesses)
{
    const ScratchDirectory cgroup{};
    Bystander first{};
    Bystander second{};
    ASSERT_GT(first.Pid(), 0);
    ASSERT_GT(second.Pid(), 0);
    ListMembers(cgroup.Path(), {first.Pid(), second.Pid()});

    EXPECT_EQ(tepegoz::ProcessGroupBuilder::KillGroup(cgroup.Path()), tepegoz::KillCode::Ok);
    EXPECT_TRUE(first.WasKilled());
    EXPECT_TRUE(second.WasKilled());
    EXPECT_FALSE(std::filesystem::exists(cgroup.Path() / "cgroup.kill"));
}

TEST(ProcessTest, KillGroupWithoutCgroupKillReachesTheCgroupsBelow)
{
    const ScratchDirectory cgroup{};
    ListMembers(cgroup.Path(), {});
    const std::filesystem::path below = cgroup.Path() / "inner" / "deeper";
    std::filesystem::create_directories(below);
    Bystander bystander{};
    ASSERT_GT(bystander.Pid(), 0);
    ListMembers(below, {bystander.Pid()});

    EXPECT_EQ(tepegoz::ProcessGroupBuilder::KillGroup(cgroup.Path()), tepegoz::KillCode::Ok);
    EXPECT_TRUE(bystander.WasKilled());
}

TEST(ProcessTest, KillGroupOfMissingCgroupReturnsNoGroup)
{
    const ScratchDirectory scratch{};
    EXPECT_EQ(tepegoz::ProcessGroupBuilder::KillGroup(scratch.Path() / "none"), tepegoz::KillCode::NoGroup);
}

TEST(ProcessTest, KillGroupOfEmptyPathLeavesTheWorkingDirectoryAlone)
{
    const ScratchDirectory scratch{};
    Bystander bystander{};
    ASSERT_GT(bystander.Pid(), 0);
    ListMembers(scratch.Path(), {bystander.Pid()});
    const WorkingDirectory here{scratch.Path()};

    EXPECT_EQ(tepegoz::ProcessGroupBuilder::KillGroup(std::filesystem::path{}), tepegoz::KillCode::NoGroup);
    EXPECT_FALSE(bystander.WasKilled(unharmed_limit));
}

TEST(ProcessTest, StopKillsWhatCgroupProcsListsOutsideTheProcessGroup)
{
    const FakeCgroupParent parent{};
    const std::filesystem::path cgroup = parent.AddService("listed");
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("listed", "tpz_steady", "")), 1);
    process.StartProcess();
    ASSERT_TRUE(HasJoined(process, cgroup));
    Bystander bystander{};
    ASSERT_GT(bystander.Pid(), 0);
    ListMembers(cgroup, {bystander.Pid()});

    ASSERT_TRUE(StopAndWait(process));
    EXPECT_TRUE(bystander.WasKilled());
}

TEST(ProcessTest, MainProcessExitKillsWhatCgroupProcsListsOutsideTheProcessGroup)
{
    const FakeCgroupParent parent{};
    const std::filesystem::path cgroup = parent.AddService("ended");
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("ended", "tpz_exit",
                                         "Args: [--code, 0, --after-ms, 50]\nRestartMode: Never\n")),
              1);
    process.StartProcess();
    ASSERT_TRUE(HasJoined(process, cgroup));
    Bystander bystander{};
    ASSERT_GT(bystander.Pid(), 0);
    ListMembers(cgroup, {bystander.Pid()});

    ASSERT_TRUE(WaitForExit(process));
    EXPECT_FALSE(process.IsStopping());
    EXPECT_TRUE(bystander.WasKilled());
}

// ---- Setting the machine's time while services run. WallClockStep moves this program's wall clock only.

TEST(ProcessTest, WallClockSetForwardLeavesTheHeartbeatCountAlone)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("stepped", "tpz_steady",
                                         "CheckHealth: true\nGracePeriodMs: 0\nHeartbeatPeriodMs: 60000\n"
                                         "MissedHeartbeatThreshold: 3\n")),
              1);
    process.StartProcess();
    process.HeartbeatReceived();
    ASSERT_TRUE(process.IsRunning());

    const std::chrono::system_clock::time_point before = std::chrono::system_clock::now();
    const WallClockStep step{std::chrono::hours{1}};
    ASSERT_GE(std::chrono::system_clock::now() - before, std::chrono::minutes{59});

    process.CheckHeartbeat();
    EXPECT_EQ(process.GetRuntimeStats().missedBeats, 0);
    EXPECT_EQ(process.GetRuntimeState(), tepegoz::RuntimeState::Running);
    EXPECT_FALSE(process.NeedsRestart());
    // The report's times are wall-clock times, so they do follow the step.
    EXPECT_GE(process.GetHealthReport().snapshotTime - before, std::chrono::minutes{59});
}

TEST(ProcessTest, WallClockSetForwardDoesNotShortenTheRestartDelay)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("early", "tpz_exit",
                                         "Args: [--code, 1, --after-ms, 0]\nRestartMode: Always\n"
                                         "RestartDelayMs: 2000\n")),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForExit(process));

    const std::chrono::system_clock::time_point before = std::chrono::system_clock::now();
    const WallClockStep step{std::chrono::hours{1}};
    ASSERT_GE(std::chrono::system_clock::now() - before, std::chrono::minutes{59});

    EXPECT_FALSE(process.NeedsRestart());
}

TEST(ProcessTest, WallClockSetBackDoesNotLengthenTheRestartDelay)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("late", "tpz_exit",
                                         "Args: [--code, 1, --after-ms, 0]\nRestartMode: Always\n"
                                         "RestartDelayMs: 300\n")),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForExit(process));

    const std::chrono::system_clock::time_point before = std::chrono::system_clock::now();
    const WallClockStep step{-std::chrono::hours{1}};
    ASSERT_LE(std::chrono::system_clock::now() - before, -std::chrono::minutes{59});

    EXPECT_TRUE(WaitUntil([&process]() { return process.NeedsRestart(); }));
}

TEST(ProcessTest, WallClockSetBackDoesNotHoldUpTheKillOfAStop)
{
    const ScratchDirectory scratch{};
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("held", "tpz_stubborn",
                                         "Args: [--period-ms, 50]\nForceKillTimeoutMs: 1000\n" +
                                             scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "parent tick"));
    process.Stop();

    const std::chrono::system_clock::time_point before = std::chrono::system_clock::now();
    const WallClockStep step{-std::chrono::hours{1}};
    ASSERT_LE(std::chrono::system_clock::now() - before, -std::chrono::minutes{59});

    EXPECT_TRUE(WaitForStop(process));
    EXPECT_FALSE(process.HasPid());
}

TEST(ProcessTest, GracePeriodIsOverBeforeTheFirstStartHoweverLongItIs)
{
    // The steady clock counts from the machine's start, which can be less than a grace period ago.
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("unstarted", "tpz_steady", "CheckHealth: true\nGracePeriodMs: 2147483647\n")),
              1);
    EXPECT_FALSE(process.IsGracePeriod());
    EXPECT_TRUE(process.NeedsStart());
}

// ---- A reload's definition, waiting for the next start

TEST(ProcessTest, DeferredDefinitionLeavesTheRunningChildAlone)
{
    const ScratchDirectory scratch{};
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("waits", "tpz_env", "Args: [--show, TPZ_OLD]\n" +
                                                               scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "env TPZ_OLD"));
    const pid_t pid = process.GetPid();
    const std::chrono::system_clock::time_point started = process.GetRuntimeStats().startTime;

    tepegoz::Process next{};
    ASSERT_EQ(Configure(next, Service("waits", "tpz_env", "Args: [--show, TPZ_NEW]\n" + scratch.FileSettings("out.txt"))),
              1);
    process.DeferDefinition(next);
    EXPECT_TRUE(process.HasDeferredDefinition());
    EXPECT_EQ(process.GetPid(), pid);
    EXPECT_TRUE(IsAlive(pid));
    EXPECT_EQ(process.GetRuntimeStats().startTime, started);
    EXPECT_EQ(process.GetRuntimeStats().restartCount, 0);
    EXPECT_EQ(process.ReapExit(), -1);
}

TEST(ProcessTest, HealthReportMarksADeferredDefinition)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("marked", "tpz_steady", "")), 1);
    EXPECT_EQ(process.GetHealthReport().runtimeHealth.restartToApply, 0);

    tepegoz::Process next{};
    ASSERT_EQ(Configure(next, Service("marked", "tpz_steady", "Args: [--period-ms, 50]\n")), 1);
    process.DeferDefinition(next);
    EXPECT_EQ(process.GetHealthReport().runtimeHealth.restartToApply, 1);
}

TEST(ProcessTest, NextStartUsesTheDeferredDefinitionAndClearsTheMark)
{
    const ScratchDirectory scratch{};
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("applies", "tpz_env", "Args: [--show, TPZ_OLD]\n" +
                                                                 scratch.FileSettings("out.txt"))),
              1);
    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "env TPZ_OLD"));

    tepegoz::Process next{};
    ASSERT_EQ(Configure(next, Service("applies", "tpz_env", "Args: [--show, TPZ_NEW]\n" +
                                                              scratch.FileSettings("out.txt"))),
              1);
    process.DeferDefinition(next);
    ASSERT_TRUE(StopAndWait(process));
    EXPECT_TRUE(process.HasDeferredDefinition());

    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "env TPZ_NEW"));
    EXPECT_FALSE(process.HasDeferredDefinition());
    EXPECT_EQ(process.GetHealthReport().runtimeHealth.restartToApply, 0);
}

TEST(ProcessTest, AutomaticRestartUsesTheDeferredDefinition)
{
    const ScratchDirectory scratch{};
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("restarted", "tpz_exit",
                                         "Args: [--code, 1, --after-ms, 300]\nRestartDelayMs: 0\nWorkMode: Auto\n")),
              1);
    process.StartProcess();

    tepegoz::Process next{};
    ASSERT_EQ(Configure(next, Service("restarted", "tpz_env", "Args: [--show, TPZ_NEW]\nWorkMode: Auto\n" +
                                                                scratch.FileSettings("out.txt"))),
              1);
    process.DeferDefinition(next);
    ASSERT_TRUE(WaitForExit(process));
    ASSERT_TRUE(WaitUntil([&process]() { return process.NeedsRestart(); }));
    process.AutoRestart();
    EXPECT_TRUE(WaitForText(scratch.Path() / "out.txt", "env TPZ_NEW"));
    EXPECT_FALSE(process.HasDeferredDefinition());
    EXPECT_EQ(process.GetRuntimeStats().restartCount, 1);
}

TEST(ProcessTest, DeferredDefinitionKeepsAStoppedServiceStopped)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("stopped", "tpz_steady", "WorkMode: Auto\nRestartMode: Always\nRestartDelayMs: 0\n")),
              1);
    process.StartProcess();
    // What --kill does: Manual, then the stop.
    process.SetWorkMode(tepegoz::WorkMode::Manual);
    ASSERT_TRUE(StopAndWait(process));

    tepegoz::Process next{};
    ASSERT_EQ(Configure(next, Service("stopped", "tpz_steady", "WorkMode: Auto\nRestartMode: Always\nArgs: [--period-ms, 50]\n")),
              1);
    process.DeferDefinition(next);
    EXPECT_FALSE(process.HasPid());
    EXPECT_FALSE(process.IsStopping());
    EXPECT_TRUE(process.IsWorkingManually());
    EXPECT_TRUE(process.HasDeferredDefinition());
}

TEST(ProcessTest, DeferredWorkModeComesWithTheDefinition)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("mode", "tpz_steady", "WorkMode: Manual\n")), 1);

    tepegoz::Process next{};
    ASSERT_EQ(Configure(next, Service("mode", "tpz_steady", "WorkMode: Auto\n")), 1);
    process.DeferDefinition(next);
    EXPECT_TRUE(process.IsWorkingManually());
    process.StartProcess();
    EXPECT_FALSE(process.IsWorkingManually());
}

TEST(ProcessTest, RestartRequestKeepsTheWorkModeItWasGiven)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("kept", "tpz_steady", "WorkMode: Auto\n")), 1);
    process.StartProcess();

    tepegoz::Process next{};
    ASSERT_EQ(Configure(next, Service("kept", "tpz_steady", "WorkMode: Manual\n")), 1);
    process.DeferDefinition(next);
    // What --restart does with a running service: Auto, then the restart.
    process.SetWorkMode(tepegoz::WorkMode::Auto);
    process.Restart();
    ASSERT_TRUE(WaitForStop(process));
    ASSERT_TRUE(process.IsRestartRequested());
    process.StartProcess();
    EXPECT_TRUE(process.HasPid());
    EXPECT_FALSE(process.HasDeferredDefinition());
    EXPECT_FALSE(process.IsWorkingManually());
}

TEST(ProcessTest, ApplyDeferredDefinitionTakesItWithoutStarting)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("now", "tpz_steady", "DependsOn: [old]\n")), 1);

    tepegoz::Process next{};
    ASSERT_EQ(Configure(next, Service("now", "tpz_steady", "DependsOn: [new]\n")), 1);
    process.DeferDefinition(next);
    process.ApplyDeferredDefinition();
    EXPECT_EQ(process.GetDependencyList(), std::vector<std::string>{"new"});
    EXPECT_FALSE(process.HasDeferredDefinition());
    EXPECT_FALSE(process.HasPid());
}

TEST(ProcessTest, LaterDeferredDefinitionReplacesAnEarlierOne)
{
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("twice", "tpz_steady", "DependsOn: [old]\n")), 1);

    tepegoz::Process first{};
    ASSERT_EQ(Configure(first, Service("twice", "tpz_steady", "DependsOn: [first]\n")), 1);
    tepegoz::Process second{};
    ASSERT_EQ(Configure(second, Service("twice", "tpz_steady", "DependsOn: [second]\n")), 1);
    process.DeferDefinition(first);
    process.DeferDefinition(second);
    process.ApplyDeferredDefinition();
    EXPECT_EQ(process.GetDependencyList(), std::vector<std::string>{"second"});
}

TEST(ProcessTest, OverrideStartupArgsReachesTheDeferredDefinition)
{
    const ScratchDirectory scratch{};
    tepegoz::Process process{};
    ASSERT_EQ(Configure(process, Service("override", "tpz_env", "Args: [--show, TPZ_OLD]\n" +
                                                                  scratch.FileSettings("out.txt"))),
              1);

    tepegoz::Process next{};
    ASSERT_EQ(Configure(next, Service("override", "tpz_env", "Args: [--show, TPZ_NEW]\n" +
                                                               scratch.FileSettings("out.txt"))),
              1);
    process.DeferDefinition(next);
    process.OverrideStartupArgs({"--show", "TPZ_OVERRIDE"});
    process.StartProcess();
    ASSERT_TRUE(WaitForText(scratch.Path() / "out.txt", "env TPZ_OVERRIDE"));
    EXPECT_EQ(ReadFile(scratch.Path() / "out.txt").find("TPZ_NEW"), std::string::npos);
}

TEST(ProcessTest, MoveConstructionCarriesTheDeferredDefinition)
{
    tepegoz::Process source{};
    ASSERT_EQ(Configure(source, Service("carried", "tpz_steady", "")), 1);
    source.StartProcess();
    const pid_t pid = source.GetPid();
    tepegoz::Process next{};
    ASSERT_EQ(Configure(next, Service("carried", "tpz_steady", "Args: [--period-ms, 50]\n")), 1);
    source.DeferDefinition(next);

    tepegoz::Process target{std::move(source)};
    EXPECT_TRUE(target.HasDeferredDefinition());
    EXPECT_FALSE(source.HasDeferredDefinition());
    EXPECT_EQ(target.GetPid(), pid);
    EXPECT_TRUE(IsAlive(pid));
}
