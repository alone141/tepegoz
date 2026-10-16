#include <Process.h>
#include <unistd.h>
#include <signal.h>
#include <thread>
#include <sys/wait.h>
#include <fstream>
#include <fcntl.h>
#include <legacy/common/logger/Log.h>
#include <ProcessGroupBuilder.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#ifndef JOURNAL_STDOUT_SOCKET
#define JOURNAL_STDOUT_SOCKET "/run/systemd/journal/stdout"
#endif

using namespace process_manager;
namespace fs = std::filesystem;

namespace
{
	constexpr size_t max_stdio_line = 1024;
	constexpr int max_stdio_reads = 16;
	constexpr std::chrono::minutes max_restart_delay{ 5 };

	// systemd sets JOURNAL_STREAM to <device>:<inode> of the journal stream behind stdout.
	bool IsStdoutJournal()
	{
		const char* stream = std::getenv("JOURNAL_STREAM");
		unsigned long long device = 0;
		unsigned long long inode = 0;
		if (stream == nullptr || std::sscanf(stream, "%llu:%llu", &device, &inode) != 2) return false;

		struct stat status {};
		if (::fstat(STDOUT_FILENO, &status) != 0) return false;
		return static_cast<unsigned long long>(status.st_dev) == device && static_cast<unsigned long long>(status.st_ino) == inode;
	}

	// The header sd_journal_stream_fd() sends: identifier, unit, priority (6, info),
	// level prefix, forward to syslog, to kmsg, to console.
	std::string JournalStreamHeader(const std::string& identifier)
	{
		return identifier + "\n\n6\n1\n0\n0\n0\n";
	}

	// The functions below run in the child between fork and exec: async-signal-safe calls only.
	void WriteAll(int fd, const char* data, size_t size)
	{
		while (size > 0)
		{
			const ssize_t written = ::write(fd, data, size);
			if (written < 0 && errno == EINTR) continue;
			if (written <= 0) return;
			data += written;
			size -= static_cast<size_t>(written);
		}
	}

	void ChildError(const char* processName, const char* what)
	{
		const char* reason = std::strerror(errno);
		for (const char* part : { "[", processName, "] ", what, ": ", reason, "\n" })
		{
			WriteAll(STDERR_FILENO, part, std::strlen(part));
		}
	}

	// stdout and stderr become a journal stream of the child's own, so journald files the
	// child's lines under its PID and cgroup instead of the manager's.
	bool ConnectJournalStream(const std::string& header)
	{
		const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0) return false;

		sockaddr_un address{};
		address.sun_family = AF_UNIX;
		std::strncpy(address.sun_path, JOURNAL_STDOUT_SOCKET, sizeof(address.sun_path) - 1);
		if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
		{
			::close(fd);
			return false;
		}
		::shutdown(fd, SHUT_RD);
		WriteAll(fd, header.data(), header.size());
		const bool redirected = ::dup2(fd, STDOUT_FILENO) != -1 && ::dup2(fd, STDERR_FILENO) != -1;
		::close(fd);
		return redirected;
	}
}

int Process::UpdateArgs(const char* sectionOrder)
{
	return config.UpdateArgs(sectionOrder);
}

Process::~Process()
{
	if (runtime.processHandle.pid > 0)
	{
		RequestStopAndEscalate();
	}
}

Process::Process(Process&& other) noexcept
	: legacy::type::IObject(std::move(other)),
	runtime(std::move(other.runtime)),
	config(std::move(other.config))
{
	other.runtime.processHandle = ProcessHandleStruct{};
}

Process& Process::operator=(Process&& other) noexcept
{
	if (this != &other)
	{
		if (HasPid())
		{
			RequestStopAndEscalate();
		}
		legacy::type::IObject::operator=(std::move(other));
		runtime = std::move(other.runtime);
		config = std::move(other.config);
		other.runtime.processHandle = ProcessHandleStruct{};
	}
	return *this;
}

void Process::StartProcess()
{
	if (HasPid())
	{
		LOGF_DEBUG("[%s] Already running as pid %d, not starting another instance", config.ProcessName.c_str(), runtime.processHandle.pid);
		return;
	}
	runtime.consecutiveRestarts = 0;
	Launch();
}

void Process::AutoRestart()
{
	if (HasPid()) return;
	++runtime.consecutiveRestarts;
	LOGF_INFO("[%s] Restart %d of at most %d in a row", config.ProcessName.c_str(), runtime.consecutiveRestarts, config.RestartPolicy.MaxRestarts);
	Launch();
}

void Process::Launch()
{
	LOGF_DEBUG("[%s] Starting service...", config.ProcessName.c_str());
	if (config.UseDisplay)
	{
		LOGF_DEBUG("[%s] Will use Display", config.ProcessName.c_str());
	}
	if (!config.StdioPolicy.StdioFilePath.empty())
	{
		LOGF_DEBUG("[%s] Logs will be stored in %s", config.ProcessName.c_str(), config.StdioPolicy.StdioFilePath.c_str());
	}

	ProcessHandleStruct pHandle;


	SetupProcessGroupSettings();

	if (config.StdioPolicy.StdioMode == StdioModeEnum::Pipe)
	{
		DrainStdioPipe(); // keep what the previous instance wrote
		FlushStdioLine();
		runtime.pipeFd.ConstructPipe();
	}

	// Prepared before fork: the child must not allocate.
	const bool useJournalStream = config.StdioPolicy.StdioMode == StdioModeEnum::Inherit && IsStdoutJournal();
	const std::string journalHeader = useJournalStream ? JournalStreamHeader(config.ProcessName) : std::string();

	pid_t pid = ::fork();
	if (pid < 0)
	{
		RemoveProcessGroup();
		runtime.pipeFd.CloseRead();
		runtime.pipeFd.CloseWrite();
		LOGF_ERROR("[%s] fork() failed with : %s", config.ProcessName.c_str(), std::strerror(errno));
	}
	if (pid == 0) //CHILD
	{
		// Its own process group, so a stop also reaches whatever it starts.
		::setpgid(0, 0);
		// The manager blocks its shutdown signals for its own threads; the child starts with none blocked.
		sigset_t noSignals;
		sigemptyset(&noSignals);
		::sigprocmask(SIG_SETMASK, &noSignals, nullptr);
		if (::isatty(STDIN_FILENO))
		{
			// Outside the terminal's foreground group a read from it would stop the child (SIGTTIN).
			const int devNull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
			if (devNull >= 0) { ::dup2(devNull, STDIN_FILENO); ::close(devNull); }
		}

		SetupWorkingDirectory();
		SetupChildGroupSettings();
		if (useJournalStream) ConnectJournalStream(journalHeader); // else it keeps the manager's output
		if (!SetupStdioSettings())
		{
			ChildError(config.ProcessName.c_str(), "Redirecting stdout and stderr failed");
			::_exit(EXIT_FAILURE);
		}
		if (!config.InheritParentEnviroment) ::clearenv();
		SetupDisplay();
		SetupEnviromentVariables();
		StartChildProcess(); //exec
	}
	if (pid > 0) //PARENT
	{
		pHandle.pid = pid;
		::setpgid(pid, pid); // as in the child: whichever runs first, the group exists before any stop
		pHandle.pgid = pid;

		if (config.StdioPolicy.StdioMode == StdioModeEnum::Pipe)
		{
			runtime.pipeFd.CloseWrite();
			if (runtime.pipeFd.readFd >= 0)
			{
				::fcntl(runtime.pipeFd.readFd, F_SETFL, ::fcntl(runtime.pipeFd.readFd, F_GETFL) | O_NONBLOCK);
			}
		}

		runtime.stopPhase = StopPhaseEnum::None;
		runtime.restartRequested = false;
		runtime.healthCheckSuspended = false;
		runtime.unresponsiveStop = false;

		if (config.HealthPolicy.CheckHealth)
		{
			GetRuntimeStatus().state = RuntimeStateEnum::Starting;
		}
		else
		{
			GetRuntimeStatus().state = RuntimeStateEnum::Running;
		}
		if (!IsUndefined(GetRuntimeStatus().startTime))
		{
			GetRuntimeStatus().restatCount++; // restarts: the first start is not one
		}
		GetRuntimeStatus().startTime = Now();
		GetRuntimeStatus().lastSeen = Now();
		GetRuntimeStatus().missedBeats = 0;
		runtime.exitStatus = ExitStatusStruct{};
	}

	runtime.processHandle = pHandle;
}

void Process::RequestStop(const ProcessHandleStruct& pHandle, int sig) const
{
	if (pHandle.pid <= 0) return;
	if (pHandle.pgid > 0)
	{
		::kill(-pHandle.pgid, sig); //@meliksahs TODO prefer CreateProcessGroup
	}
	else
	{
		LOGF_VERBOSE("[%s] RequestStop pid %d with sig no %d", config.ProcessName.c_str(), pHandle.pid, sig);
		::kill(pHandle.pid, sig);
	}
}

void Process::ForceStop(const ProcessHandleStruct& pHandle) const
{
	RequestStop(pHandle, SIGKILL);
}

// Blocking: only for shutdown and destruction. The supervision loop uses Stop() and AdvanceStop().
void Process::RequestStopAndEscalate(std::chrono::milliseconds poll)
{
	LOGF_DEBUG("[%s] RequestStopAndEscalate with polling period %ld ms and ForceKilTimeout %ld ms ", config.ProcessName.c_str(), static_cast<long>(poll.count()), static_cast<long>(config.ForceKillTimeoutMs.count()));
	if (runtime.processHandle.pid <= 0) return;

	poll = std::max(poll, std::chrono::milliseconds(1));
	Stop();
	while (IsStopping())
	{
		std::this_thread::sleep_for(poll);
		AdvanceStop();
	}
}

void Process::Stop()
{
	runtime.restartRequested = false;
	BeginStop();
}

void Process::Restart()
{
	runtime.restartRequested = true;
	BeginStop();
}

// Stopped for missed heartbeats: the exit counts as a failure, and the restart policy restarts it.
void Process::StopUnresponsive()
{
	LOGF_ERROR("[%s] Unresponsive: %d heartbeat periods without a heartbeat, stopping it", config.ProcessName.c_str(), runtime.runtimeHealth.missedBeats);
	runtime.restartRequested = false;
	runtime.unresponsiveStop = true;
	BeginStop();
}

void Process::BeginStop()
{
	if (!HasPid() || IsStopping()) return;

	LOGF_DEBUG("[%s] Stopping pid %d, SIGKILL after %ld ms", config.ProcessName.c_str(), runtime.processHandle.pid, static_cast<long>(config.ForceKillTimeoutMs.count()));
	runtime.stopGroup = runtime.processHandle.pgid;
	RequestStop(runtime.processHandle, SIGTERM);
	runtime.stopPhase = StopPhaseEnum::Terminating;
	runtime.stopDeadline = Now() + config.ForceKillTimeoutMs;
}

// A stop ends when the process and everything left in its process group are gone.
void Process::AdvanceStop()
{
	if (!IsStopping()) return;

	ReapExit();
	if (!HasPid() && !IsGroupAlive())
	{
		FinishStop();
		return;
	}
	if (Now() < runtime.stopDeadline) return;

	if (runtime.stopPhase == StopPhaseEnum::Terminating)
	{
		LOGF_DEBUG("[%s] Graceful shutdown failed ", config.ProcessName.c_str());
		if (runtime.stopGroup > 0) ::kill(-runtime.stopGroup, SIGKILL);
		else ForceStop(runtime.processHandle);
		if (config.CreateProcessGroup) TerminateProcessGroup();
		runtime.stopPhase = StopPhaseEnum::Killing;
		runtime.stopDeadline = Now() + config.ForceKillTimeoutMs;
		return;
	}

	LOGF_ERROR("[%s] Force shutdown failed", config.ProcessName.c_str());
	runtime.stopPhase = StopPhaseEnum::None;
	runtime.stopGroup = -1;
}

bool Process::IsGroupAlive() const
{
	return runtime.stopGroup > 0 && ::kill(-runtime.stopGroup, 0) == 0;
}

void Process::FinishStop()
{
	runtime.stopPhase = StopPhaseEnum::None;
	runtime.stopGroup = -1;
	if (config.CreateProcessGroup)
	{
		TerminateProcessGroup(); // whatever the main process left behind in its cgroup
		RemoveProcessGroup();
	}
	LOGF_DEBUG("[%s] Reaped & stop successful ", config.ProcessName.c_str());
}

void Process::OverrideStartupArgs(std::vector<std::string> args)
{
	config.Args = args;
}

void process_manager::Process::SetWorkMode(WorkModeEnum workMode)
{
	config.WorkMode = workMode;
}

legacy::object::ObjectType Process::GetType() const
{
	return legacy::object::Process;
}

void Process::HeartbeatReceived()
{
	LOGF_DEBUG("[%s] Heartbeat received", config.ProcessName.c_str());

	if (!IsHealthCheckActive()) return;
	if (!HasPid()) return; // late, from an instance that has exited

	if (IsUndefined(GetRuntimeStatus().startTime))
	{
		GetRuntimeStatus().startTime = Now();
	}
	GetRuntimeStatus().lastSeen = Now();
	GetRuntimeStatus().state = RuntimeStateEnum::Running;
	GetRuntimeStatus().missedBeats = 0;
}

void Process::CheckHeartbeat()
{
	if (!HasPid()) return; // an exited process is Stopped, not Unhealthy
	if (IsGracePeriod()) return;
	if (!IsHealthCheckActive()) return;
	if (IsUndefined(GetRuntimeStatus().lastSeen)) return;
	if (config.HealthPolicy.HeartbeatPeriodMs.count() <= 0) return;

	// Whole heartbeat periods since the last heartbeat: 0 while it keeps up.
	GetRuntimeStatus().missedBeats = (int)((Now() - GetRuntimeStatus().lastSeen) / config.HealthPolicy.HeartbeatPeriodMs);
	if (GetRuntimeStatus().missedBeats > 0)
	{
		LOGF_DEBUG("[%s] Heartbeat missed", config.ProcessName.c_str());
	}
	if (GetRuntimeStatus().missedBeats >= config.HealthPolicy.MissedHeartbeatThreshold)
	{
		GetRuntimeStatus().state = RuntimeStateEnum::Unhealthy;
	}
}

std::string Process::GetProcessName() const
{
	return config.ProcessName;
}

std::vector<std::string> Process::GetDependencyList() const
{
	return config.DependsOn;
}

RuntimeStateEnum Process::GetRuntimeState() const
{
	return runtime.runtimeHealth.state;
}

uint64_t process_manager::Process::GetMemoryUsageInBytes() const
{
	std::ifstream memoryFile(runtime.cgroup_path / "memory.current");
	uint64_t memUsage = 0;
	if (memoryFile >> memUsage)
	{
		return memUsage;
	}
	return 0; //error?
}

uint64_t process_manager::Process::GetCpuUsageInUsec() const
{
	std::ifstream file(runtime.cgroup_path / "cpu.stat");
	std::string line;
	while (std::getline(file, line))
	{
		if (line.rfind("usage_usec", 0) == 0)
		{
			std::istringstream iss(line);
			std::string key;
			uint64_t val;
			if (iss >> key >> val)
			{
				return val;
			}
		}
	}
	return 0;
}

RuntimeHealthStruct& Process::GetRuntimeStatus()
{
	return runtime.runtimeHealth;
}

bool Process::IsGracePeriod() const
{
	const bool isGrace = (Now() - runtime.runtimeHealth.startTime) < config.HealthPolicy.GracePeriodMs;
	return isGrace;
}

bool Process::IsRunning() const
{
	return runtime.runtimeHealth.state == RuntimeStateEnum::Running;
}

bool Process::IsStarting() const
{
	return runtime.runtimeHealth.state == RuntimeStateEnum::Starting;
}

bool Process::IsStopping() const
{
	return runtime.stopPhase != StopPhaseEnum::None;
}

bool Process::IsRestartRequested() const
{
	return runtime.restartRequested;
}

bool Process::IsHealthCheckActive() const
{
	return config.HealthPolicy.CheckHealth && !runtime.healthCheckSuspended;
}

bool Process::ExitedWithFailure() const
{
	const auto& exitStatus = runtime.exitStatus;
	if (exitStatus.Pid <= 0) return false; // no exit recorded since the last start
	if (runtime.unresponsiveStop) return true; // we stopped it for missed heartbeats

	if (!exitStatus.IsTerminatedBySignal) return exitStatus.ExitCode > 0;

	// The signals systemd's Restart=on-failure counts as a clean stop.
	switch (exitStatus.TermSignal)
	{
	case SIGHUP:
	case SIGINT:
	case SIGTERM:
	case SIGPIPE:
		return false;
	default:
		return true;
	}
}

bool Process::IsUnresponsive() const
{
	return IsHealthCheckActive() && runtime.runtimeHealth.state == RuntimeStateEnum::Unhealthy;
}

bool Process::RestartLimitReached() const
{
	return runtime.consecutiveRestarts >= config.RestartPolicy.MaxRestarts;
}

// RestartDelayMs before the first restart in a row, BackoffMultiplier times longer for each one after.
std::chrono::milliseconds Process::RestartDelay() const
{
	const auto& policy = config.RestartPolicy;
	const double delayMs = static_cast<double>(policy.RestartDelayMs.count()) * std::pow(policy.BackoffMultiplier, runtime.consecutiveRestarts);
	const double capMs = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(max_restart_delay).count());
	return std::chrono::milliseconds(static_cast<long long>(std::min(delayMs, capMs)));
}

// For a running process: restart it when it stopped answering (systemd's watchdog counts that as a
// failure too). For an exited one: per RestartMode, once the backoff delay since the exit has passed.
bool Process::NeedsRestart() const
{
	//Orumcek agina dondu burasi
	const auto rMode = config.RestartPolicy.RestartMode;
	if (rMode == RestartModeEnum::Never) return false;
	if (RestartLimitReached()) return false;

	if (HasPid())
	{
		return !IsGracePeriod() && IsUnresponsive();
	}
	if (IsUndefined(runtime.exitTime)) return false; // never ran: NeedsStart covers it
	if (Now() - runtime.exitTime < RestartDelay()) return false;
	if (rMode == RestartModeEnum::OnFailure)
	{
		return ExitedWithFailure();
	}
	return true;
}

bool Process::NeedsStart() const
{
	if (IsUndefined(runtime.runtimeHealth.startTime))
	{
		return true;
	}
	return false;
}

bool process_manager::Process::IsWorkingManually() const
{
	return WorkModeEnum::Manual == config.WorkMode;
}

bool Process::HasPid() const
{
	return runtime.processHandle.pid > 0;
}

// The main process, or one in its process group. With a cgroup, also anything under it, as its memory
// and CPU use count: a process that left the group, or outlived the main process, still belongs to it.
bool Process::IsMember(pid_t pid, pid_t pgid, const fs::path& cgroup) const
{
	if (HasPid() && (pid == runtime.processHandle.pid || (pgid > 0 && pgid == runtime.processHandle.pgid))) return true;
	if (!config.CreateProcessGroup || runtime.cgroup_path.empty() || cgroup.empty()) return false;

	const auto& own = runtime.cgroup_path;
	return std::mismatch(own.begin(), own.end(), cgroup.begin(), cgroup.end()).first == own.end();
}

pid_t Process::GetPid() const
{
	return runtime.processHandle.pid;
}

void Process::CheckConfig(std::vector<std::string>& errorList) const noexcept
{
	std::error_code erc;

	//check executablepath

	if (!fs::exists(config.ExecutablePath, erc))
	{
		errorList.push_back(GetProcessName() + " ExecutablePath does not exists");
	}
	if (fs::is_directory(config.ExecutablePath, erc))
	{
		errorList.push_back(GetProcessName() + " ExecutablePath is a directory");
	}
	if (!fs::is_regular_file(config.ExecutablePath, erc))
	{
		errorList.push_back(GetProcessName() + " ExecutablePath is not a regular file");
	}
	if (::access(config.ExecutablePath.c_str(), X_OK) != 0)
	{
		errorList.push_back(GetProcessName() + " ExecutablePath access denied");
	}

	const auto& resolvedPath = fs::weakly_canonical(config.ExecutablePath, erc);
	if (!fs::exists(config.ExecutablePath, erc))
	{
		errorList.push_back(GetProcessName() + " ResolvedPath does not exists");
	}
	if (!fs::is_regular_file(config.ExecutablePath, erc))
	{
		errorList.push_back(GetProcessName() + " ResolvedPath is not a regular file");
	}


	//check cwd
	// 1) Must exist
	if (!config.Cwd.empty())
	{
		if (!fs::exists(config.Cwd, erc))
		{
			errorList.push_back(GetProcessName() + " Cwd does not exists");
		}

		// 2) Must be a directory
		if (!fs::is_directory(config.Cwd, erc))
		{
			errorList.push_back(GetProcessName() + " Cwd is not a directory");
		}
	}



	//check StdioFilePath
	if (config.StdioPolicy.StdioMode != StdioModeEnum::File) return;
	if (config.StdioPolicy.StdioFilePath.empty())
	{
		errorList.push_back(GetProcessName() + " StdioFilePath is empty logging will fail");
		return;
	}

	// The file itself is created on the first start; its directory has to be there.
	const auto logDirectory = config.StdioPolicy.StdioFilePath.parent_path();
	if (!logDirectory.empty() && !fs::is_directory(logDirectory, erc))
	{
		errorList.push_back(GetProcessName() + " StdioFilePath directory does not exists");
	}

	return;
}

void Process::SuspendHealthCheck()
{
	runtime.healthCheckSuspended = true;
}

RuntimeHealthStruct Process::GetRuntimeStats() const noexcept
{
	return runtime.runtimeHealth;
}


pid_t Process::ReapExit()
{
	const auto curentPid = runtime.processHandle.pid;
	ExitStatusStruct exits;

	if (curentPid <= 0) return 0;

	while (true)
	{
		int status = 0;
		//meliksahs this might occasionally hang 
		//even with WNOHANG
		pid_t pid = ::waitpid(curentPid, &status, WNOHANG);

		if (pid < 0 && errno == ECHILD)
		{
			// No longer our child. Drop the pid, which would otherwise signal whoever reuses it;
			// the exit status stays unknown (the defaults).
			pid = curentPid;
		}
		else if (pid <= 0)
		{
			break;
		}
		else if (WIFEXITED(status))
		{
			exits.IsTerminatedBySignal = false;
			exits.ExitCode = WEXITSTATUS(status);
		}
		else if (WIFSIGNALED(status))
		{
			exits.IsTerminatedBySignal = true;
			exits.TermSignal = WTERMSIG(status);
		}

		exits.Pid = pid;
		runtime.processHandle.pid = -1;
		runtime.processHandle.pgid = -1;
		runtime.exitStatus = exits;
		GetRuntimeStatus().state = RuntimeStateEnum::Stopped;

		runtime.exitTime = Now();
		if (runtime.exitTime - GetRuntimeStatus().startTime >= config.RestartPolicy.RestartResetMs)
		{
			runtime.consecutiveRestarts = 0; // it ran long enough: a new failure starts a new series
		}
		if (!exits.IsTerminatedBySignal)
		{
			LOGF_INFO("[%s] pid %d exited with code %d", config.ProcessName.c_str(), pid, exits.ExitCode);
		}
		else if (exits.TermSignal > 0)
		{
			LOGF_INFO("[%s] pid %d terminated by signal %d", config.ProcessName.c_str(), pid, exits.TermSignal);
		}
		else
		{
			LOGF_INFO("[%s] pid %d is gone, exit status unknown", config.ProcessName.c_str(), pid);
		}

		const auto rMode = config.RestartPolicy.RestartMode;
		if (!IsWorkingManually() && rMode != RestartModeEnum::Never && RestartLimitReached() &&
			(rMode != RestartModeEnum::OnFailure || ExitedWithFailure()))
		{
			LOGF_ERROR("[%s] Not restarting: %d restarts in a row (MaxRestarts)", config.ProcessName.c_str(), runtime.consecutiveRestarts);
		}
		return pid;

	}
	return -1;
}

DetailedHealthReportStruct Process::GetHealthReport() const
{
	// Sent as raw bytes: zero the padding too, and keep the name NUL-terminated.
	DetailedHealthReportStruct healthReport;
	std::memset(static_cast<void*>(&healthReport), 0, sizeof(healthReport));
	const size_t nameLength = std::min(config.ProcessName.size(), sizeof(healthReport.processName) - 1);
	std::memcpy(healthReport.processName, config.ProcessName.data(), nameLength);

	const auto& health = runtime.runtimeHealth;
	healthReport.runtimeHealth.state = health.state;
	healthReport.runtimeHealth.startTime = health.startTime;
	healthReport.runtimeHealth.lastSeen = health.lastSeen;
	healthReport.runtimeHealth.missedBeats = health.missedBeats;
	healthReport.runtimeHealth.restatCount = health.restatCount;
	healthReport.pid = GetPid();
	healthReport.memoryUsageInBytes = GetMemoryUsageInBytes();
	healthReport.cpuUsageInUsec = GetCpuUsageInUsec();
	healthReport.snapshotTime = Now();
	return healthReport;
}

void Process::DrainStdioPipe()
{
	char buffer[4096];
	for (int reads = 0; runtime.pipeFd.readFd >= 0 && reads < max_stdio_reads; ++reads)
	{
		const ssize_t count = ::read(runtime.pipeFd.readFd, buffer, sizeof(buffer));
		if (count > 0)
		{
			AppendStdioOutput(std::string_view(buffer, static_cast<size_t>(count)));
			continue;
		}
		if (count < 0 && errno == EINTR) continue;
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;

		// End of file: every writer is gone.
		FlushStdioLine();
		runtime.pipeFd.CloseRead();
	}
}

void Process::AppendStdioOutput(std::string_view output)
{
	auto& line = runtime.stdioLine;
	line.append(output.data(), output.size());

	size_t start = 0;
	for (size_t newline = line.find('\n'); newline != std::string::npos; newline = line.find('\n', start))
	{
		const std::string text = line.substr(start, newline - start);
		LOGF_INFO("[%s] %s", config.ProcessName.c_str(), text.c_str());
		start = newline + 1;
	}
	line.erase(0, start);

	if (line.size() >= max_stdio_line) FlushStdioLine();
}

void Process::FlushStdioLine()
{
	if (runtime.stdioLine.empty()) return;
	LOGF_INFO("[%s] %s", config.ProcessName.c_str(), runtime.stdioLine.c_str());
	runtime.stdioLine.clear();
}

bool Process::SetupStdioPipe() const
{
	const auto& pipe = runtime.pipeFd;
	::close(pipe.readFd);

	// Redirect stdout and stderr to the write end of the pipe
	if (::dup2(pipe.writeFd, STDOUT_FILENO) == -1 || ::dup2(pipe.writeFd, STDERR_FILENO) == -1)
	{
		return false;
	}

	// Close the original write end fd
	::close(pipe.writeFd);
	return true;
}
bool Process::SetupStdioFile(const char* path) const
{
	const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (config.StdioPolicy.StdioAppend ? O_APPEND : O_TRUNC);
	const int fd = ::open(path, flags, 0644);
	if (fd < 0) return false;

	// Redirect stdout and stderr to the file
	const bool redirected = ::dup2(fd, STDOUT_FILENO) != -1 && ::dup2(fd, STDERR_FILENO) != -1;
	::close(fd);
	return redirected;
}
std::vector<char*> Process::SetupExecParams() const
{
	std::vector<char*> argv_c;
	argv_c.push_back(const_cast<char*>(config.ExecutablePath.c_str()));
	for (const auto& arg : config.Args)
	{
		argv_c.push_back(const_cast<char*>(arg.data()));
	}
	argv_c.push_back(nullptr);

	return argv_c;
}
bool Process::SetupWorkingDirectory() const
{
	const auto& cwd = config.Cwd;

	if (!cwd.empty())
	{
		if (::chdir(cwd.c_str()) != 0)
		{
			LOGF_DEBUG("[%s] chdir failed with %s", config.ProcessName.c_str(), std::strerror(errno));
		}
	}

	return true;
}
bool process_manager::Process::SetupEnviromentVariables()
{
	for (auto& arg : config.EnviromentVariables)
	{
		::putenv(arg.data());
	}
	////meliksahs FIX
	//if (config.ProcessName == "GGM" || config.ProcessName == "GGMO")
	//{
	//	if (setenv("PYTHONPATH", "/usr/local/lib/python3.10/site-packages", 1) != 0)
	//	{
	//		LOGF_DEBUG("[%s] setenv failed with %s", config.ProcessName.c_str(), std::strerror(errno));
	//	}
	//}

	return true;
}
bool process_manager::Process::SetupDisplay() const
{
	if (!config.UseDisplay) return true;

	if (setenv("DISPLAY", config.Display.c_str(), 1) != 0)
	{
		ChildError(config.ProcessName.c_str(), "Setting DISPLAY failed");
	}
	if (!config.XAuthority.empty() && setenv("XAUTHORITY", config.XAuthority.c_str(), 1) != 0)
	{
		ChildError(config.ProcessName.c_str(), "Setting XAUTHORITY failed");
	}
	return true;
}
bool Process::SetupStdioSettings() const
{
	const auto& stdio = config.StdioPolicy;

	switch (stdio.StdioMode)
	{
	case StdioModeEnum::Inherit:
	{
		return true;
	}
	case StdioModeEnum::Pipe:
	{
		return SetupStdioPipe();
	}
	case StdioModeEnum::NullDevice:
	{
		return SetupStdioFile("/dev/null");
	}
	case StdioModeEnum::File:
	{
		return SetupStdioFile(stdio.StdioFilePath.c_str());
	}
	default:
		LOGF_DEBUG("[%s] Unknown stdiomode enum assuming ::inherit", config.ProcessName.c_str(), std::strerror(errno));
		return true;
		break;
	}

	return false;
}

bool Process::SetupChildGroupSettings() const
{
	if (!config.CreateProcessGroup) return true;

	std::string procs_file = (runtime.cgroup_path / "cgroup.procs").string();
	std::string pid_str = std::to_string(getpid());

	int fd = ::open(procs_file.c_str(), O_WRONLY);
	if (fd < 0 || write(fd, pid_str.c_str(), pid_str.length()) < 0)
	{
		LOGF_ERROR("[%s] write() failed with : %s", config.ProcessName.c_str(), std::strerror(errno));
		_exit(EXIT_FAILURE); // Immediate exit, bypass C++ destructors
	}
	close(fd);
	return true;
}

bool Process::SetupProcessGroupSettings()
{
	if (!config.CreateProcessGroup) return true;

	fs::path managerRoot = ProcessGroupBuilder::GetInstance().GetParentGroupPath();
	runtime.cgroup_path = managerRoot / ("task_" + config.ProcessName);

	std::error_code ec;
	if (!fs::create_directories(runtime.cgroup_path, ec) && ec)
	{
		LOGF_ERROR("[%s] Failed to create cgroup directory:  %s ", config.ProcessName.c_str(), std::strerror(errno));
		LOGF_ERROR("[%s] Starting directly... ", config.ProcessName.c_str(), std::strerror(errno));
		config.CreateProcessGroup = false;
		return false;
	}
	return true;
}

bool Process::TerminateProcessGroup() const
{
	if (!config.CreateProcessGroup) return true;

	fs::path kill_file = runtime.cgroup_path / "cgroup.kill";
	std::ofstream kill_stream(kill_file);
	if (kill_stream.is_open())
	{
		kill_stream << "1" << std::flush;
		kill_stream.close();
	}
	else
	{
		return false;
	}
	return true;

}

bool Process::RemoveProcessGroup() const
{
	if (!config.CreateProcessGroup) return true;

	std::error_code ec;
	if (!fs::remove(runtime.cgroup_path, ec) && ec)
	{
		LOGF_ERROR("[%s] Failed to delete cgroup directory:  %s", config.ProcessName.c_str(), std::strerror(errno));
		return false;
	}
	return true;
}

bool Process::StartChildProcess() const
{
	::execvp(config.ExecutablePath.c_str(), SetupExecParams().data());
	// exec failed. _exit: the parent's atexit handlers and static destructors must not run in this copy.
	ChildError(config.ProcessName.c_str(), "exec failed");
	::_exit(127);
}
