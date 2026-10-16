#include <Process.h>
#include <unistd.h>
#include <signal.h>
#include <thread>
#include <sys/wait.h>
#include <fstream>
#include <fcntl.h>
#include <legacy/common/logger/Log.h>
#include <ProcessGroupBuilder.h>

using namespace process_manager;
namespace fs = std::filesystem;

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

void Process::StartProcess()
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
		runtime.pipeFd.ConstructPipe();
	}

	pid_t pid = ::fork();
	if (pid < 0)
	{
		RemoveProcessGroup();
		LOGF_ERROR("[%s] fork() failed with : %s", config.ProcessName.c_str(), std::strerror(errno));
	}
	if (pid == 0) //CHILD
	{

		SetupWorkingDirectory();
		SetupChildGroupSettings();
		SetupStdioSettings();
		SetupDisplay();
		SetupEnviromentVariables();
		StartChildProcess(); //exec
	}
	if (pid > 0) //PARENT
	{
		pHandle.pid = pid;

		if (config.StdioPolicy.StdioMode == StdioModeEnum::Pipe)
		{
			::close(runtime.pipeFd.writeFd);
		}

		if (config.HealthPolicy.CheckHealth)
		{
			GetRuntimeStatus().state = RuntimeStateEnum::Starting;
		}
		else
		{
			GetRuntimeStatus().state = RuntimeStateEnum::Running;
		}
		GetRuntimeStatus().startTime = Now();
		GetRuntimeStatus().lastSeen = Now();
		GetRuntimeStatus().restatCount++;
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

void Process::RequestStopAndEscalate(std::chrono::milliseconds poll)
{
	LOGF_DEBUG("[%s] RequestStopAndEscalate with polling period %d ms and ForceKilTimeout %d ms ", config.ProcessName.c_str(), poll.count(), config.ForceKillTimeoutMs.count());
	if (runtime.processHandle.pid <= 0) return;

	//Graceful
	RequestStop(runtime.processHandle, SIGTERM);
	auto deadline = Now() + config.ForceKillTimeoutMs;
	while (Now() < deadline)
	{
		if (ReapExit() >= 0)
		{
			LOGF_DEBUG("[%s] Reaped & Graceful stop successful ", config.ProcessName.c_str());
			break;
			//return;
		}
		std::this_thread::sleep_for(poll);
	}
	if(Now() > deadline) LOGF_DEBUG("[%s] Graceful shutdown failed ", config.ProcessName.c_str());
	//LOGF_DEBUG("[%s] Graceful shutdown failed", config.ProcessName.c_str());

	//Force
	ForceStop(runtime.processHandle);
	deadline = Now() + config.ForceKillTimeoutMs;

	if (config.CreateProcessGroup) TerminateProcessGroup();

	while (Now() < deadline)
	{
		if (ReapExit() >= 0)
		{
			RemoveProcessGroup();
			LOGF_DEBUG("[%s] Reaped & Force stop successful ", config.ProcessName.c_str());
			return;
		}
		std::this_thread::sleep_for(poll);
	}
	LOGF_ERROR("[%s] Force shutdown failed", config.ProcessName.c_str());

	return;
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

	if (!config.HealthPolicy.CheckHealth) return;

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
	if (IsGracePeriod()) return;
	if (!config.HealthPolicy.CheckHealth) return;
	if (IsUndefined(GetRuntimeStatus().lastSeen)) return;

	GetRuntimeStatus().missedBeats = 1 + (int)((Now() - GetRuntimeStatus().lastSeen) / config.HealthPolicy.HeartbeatPeriodMs);
	if (GetRuntimeStatus().missedBeats > 1)
	{
		LOGF_DEBUG("[%s] Heartbeat missed", config.ProcessName.c_str());
	}
	if (GetRuntimeStatus().missedBeats - 1 > config.HealthPolicy.MissedHeartbeatThreshold)
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

bool Process::NeedsRestart() const
{
	//Orumcek agina dondu burasi
	const auto rMode = config.RestartPolicy.RestartMode;
	if (rMode == RestartModeEnum::Never) return false;
	if (runtime.runtimeHealth.restatCount > config.RestartPolicy.MaxRestarts)
	{
		return false;
	}
	if (IsGracePeriod())
	{
		return false;
	}
	//if (!config.HealthPolicy.CheckHealth)
	//{
	//	return false;
	//}
	if (rMode == RestartModeEnum::OnFailure)
	{
		if (runtime.exitStatus.ExitCode > 0)
		{
			return true;
		}
		else return false;
	}
	if (runtime.processHandle.pid < 0)
	{
		return true;
	}

	bool noHeartbeat = runtime.runtimeHealth.missedBeats >= config.HealthPolicy.MissedHeartbeatThreshold;
	return noHeartbeat;
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

	if (!fs::exists(config.StdioPolicy.StdioFilePath, erc))
	{
		errorList.push_back(GetProcessName() + " StdioFilePath does not exists");
	}

	return;
}

void Process::DisableHealthCheck()
{
	config.HealthPolicy.CheckHealth = false;
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

		if (pid <= 0) break;

		exits.Pid = pid;

		if (WIFEXITED(status))
		{
			exits.IsTerminatedBySignal = false;
			exits.ExitCode = WEXITSTATUS(status);
		}
		else if (WIFSIGNALED(status))
		{
			exits.IsTerminatedBySignal = true;
			exits.TermSignal = WTERMSIG(status);
		}

		runtime.processHandle.pid = -1;
		runtime.processHandle.pgid = -1;
		runtime.exitStatus = exits;
		GetRuntimeStatus().state = RuntimeStateEnum::Stopped;
		return pid;

	}
	return -1;
}

DetailedHealthReportStruct Process::GetHealthReport() const
{
	DetailedHealthReportStruct healthReport;
	std::memcpy(healthReport.processName, config.ProcessName.c_str(), sizeof(healthReport.processName));
	healthReport.runtimeHealth = GetRuntimeStats();
	healthReport.pid = GetPid();
	healthReport.memoryUsageInBytes = GetMemoryUsageInBytes();
	healthReport.cpuUsageInUsec = GetCpuUsageInUsec();
	healthReport.snapshotTime = Now();
	return healthReport;
}

bool Process::SetupStdioPipe() const
{
	const auto& pipe = runtime.pipeFd;
	::close(pipe.readFd);

	// Redirect stdout (fd 1) to the write end of the pipe
	if (::dup2(pipe.writeFd, STDOUT_FILENO) == -1)
	{
		return false;
	}

	// Close the original write end fd
	::close(pipe.writeFd);
	return true;
}
bool Process::SetupStdioFile(const char* path) const
{
	const auto& stdio = config.StdioPolicy;

	if (stdio.StdioAppend == true)
	{
		freopen(path, "a", stdout); //meliksahs TODO bu onerilmiyor
	}
	else
	{
		freopen(path, "w", stdout); //meliksahs TODO bu onerilmiyor
	}
	return true;
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
std::vector<char*> process_manager::Process::SetupEnviromentParams() const
{
	std::vector<char*> enviroment;
	if (config.UseDisplay)
	{
		//meliksahs fix kotu oldu
		enviroment.push_back("DISPLAY=:0");
		enviroment.push_back("DISPLAY=:1");
		enviroment.push_back("XAUTHORITY=/home/user/.Xauthority");
	}
	for (const auto& arg : config.EnviromentVariables)
	{
		enviroment.push_back(const_cast<char*>(arg.data()));
	}
	enviroment.push_back(nullptr);

	return enviroment;
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
	LOGF_DEBUG("[%s] Will use Display", config.ProcessName.c_str());

	if (setenv("DISPLAY", ":0", 1) != 0)
	{
		LOGF_DEBUG("[%s] setenv failed with %s", config.ProcessName.c_str(), std::strerror(errno));
	}
	if (setenv("DISPLAY", ":1", 1) != 0)
	{
		LOGF_DEBUG("[%s] setenv failed with %s", config.ProcessName.c_str(), std::strerror(errno));
	}
	if (setenv("XAUTHORITY", "/home/user/.Xauthority", 1) != 0)
	{
		LOGF_DEBUG("[%s] setenv failed with %s", config.ProcessName.c_str(), std::strerror(errno));
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
		SetupStdioPipe();
		return true;
	}
	case StdioModeEnum::NullDevice:
	{
		SetupStdioFile("/dev/null");
		return true;
	}
	case StdioModeEnum::File:
	{
		SetupStdioFile(stdio.StdioFilePath.c_str());
		return true;
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
	::exit(EXIT_FAILURE); //exec failed
}
