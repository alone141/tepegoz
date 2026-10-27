#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <queue>

#include <ProcessManager.h>
#include <PipeStruct.h>

#include <zmq_addon.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include <ProcessGroupBuilder.h>
#include "ConfigSection.hpp"
#include "GpuMonitor.hpp"
#include "JournalServer.hpp"
#include "Logger.hpp"
using namespace process_manager;

namespace
{
	constexpr int max_messages_per_cycle = 64;
	// A GPU sample this old means the sampling thread is stuck in a driver call.
	constexpr std::chrono::milliseconds gpu_sample_stale_after{ 5'000 };
	// Reports go out on this schedule, not on every pass: each heartbeat wakes the loop, so a report per pass made
	// every subscriber's traffic grow with the square of the number of services.
	constexpr std::chrono::milliseconds report_period{ 500 };

	// Whether two reports show the same services in the same states. A change goes out at once instead of at the
	// next scheduled report, so a start, stop or restart is seen even when it is over within one period.
	bool SameStates(const std::vector<process_manager::DetailedHealthReportStruct>& a, const std::vector<process_manager::DetailedHealthReportStruct>& b)
	{
		return std::equal(a.begin(), a.end(), b.begin(), b.end(),
			[](const process_manager::DetailedHealthReportStruct& x, const process_manager::DetailedHealthReportStruct& y)
			{
				return x.pid == y.pid && x.runtimeHealth.state == y.runtimeHealth.state
					&& x.runtimeHealth.restatCount == y.runtimeHealth.restatCount
					&& std::strncmp(x.processName, y.processName, sizeof(x.processName)) == 0;
			});
	}

	std::string FixedString(const char* text, size_t capacity)
	{
		return std::string(text, ::strnlen(text, capacity));
	}

	// The manager's unit, and the cgroup that holds task_pm and every task_<name>, as journald records them.
	journal_server::ManagerIdentity JournalIdentity()
	{
		const std::filesystem::path parent = ProcessGroupBuilder::GetInstance().GetParentGroupPath();
		const std::filesystem::path relative = parent.lexically_relative("/sys/fs/cgroup");
		const bool inside = !relative.empty() && relative != "." && *relative.begin() != "..";
		return journal_server::IdentityFromCgroup(inside ? "/" + relative.generic_string() : "/", ::getpid());
	}

	// A process on a GPU, with what tells which service it belongs to.
	struct GpuProcess
	{
		pid_t pid;
		pid_t pgid;
		std::filesystem::path cgroup;
		gpu_monitor::ProcessGpuUsage usage;
	};

	// Turns the GPU samples, which are per process, into GPU use per service for the health reports.
	class GpuUsageReporter
	{
	public:
		void Update(const gpu_monitor::GpuSnapshot& gpu)
		{
			state = GetState(gpu);
			if (state != loggedState)
			{
				Log(gpu);
				loggedState = state;
			}
			if (gpu.takenAt == takenAt) return; // the sample of the last pass
			takenAt = gpu.takenAt;
			processes.clear();
			for (const auto& entry : gpu.processes)
			{
				processes.push_back({ entry.first, ::getpgid(entry.first), ProcessGroupBuilder::GetCgroupPathOf(entry.first), entry.second });
			}
		}

		process_manager::GpuUsageStruct GetUsage(const Process& process) const
		{
			process_manager::GpuUsageStruct usage{};
			usage.state = state;
			if (state != process_manager::GpuUsageStateEnum::Sampled) return usage;

			bool utilizationKnown = false;
			bool utilizationMissing = false;
			int32_t utilization = 0;
			for (const auto& gpuProcess : processes)
			{
				if (!process.IsMember(gpuProcess.pid, gpuProcess.pgid, gpuProcess.cgroup)) continue;
				usage.memoryUsageInBytes += gpuProcess.usage.memoryBytes;
				if (gpuProcess.usage.utilizationPercent == gpu_monitor::utilization_unknown)
				{
					utilizationMissing = true;
					continue;
				}
				utilizationKnown = true;
				utilization += gpuProcess.usage.utilizationPercent;
			}
			// Unknown only when the driver reports it for none of its GPU processes; without any it uses none.
			usage.utilizationPercent = utilizationMissing && !utilizationKnown ? -1 : utilization;
			return usage;
		}

	private:
		static process_manager::GpuUsageStateEnum GetState(const gpu_monitor::GpuSnapshot& gpu)
		{
			if (gpu.status == gpu_monitor::GpuStatus::NotSampled) return process_manager::GpuUsageStateEnum::NotSampled;
			if (std::chrono::steady_clock::now() - gpu.takenAt > gpu_sample_stale_after) return process_manager::GpuUsageStateEnum::Stalled;
			switch (gpu.status)
			{
			case gpu_monitor::GpuStatus::Ok:
				return process_manager::GpuUsageStateEnum::Sampled;
			case gpu_monitor::GpuStatus::LibraryMissing:
				return process_manager::GpuUsageStateEnum::NoDriver;
			case gpu_monitor::GpuStatus::NoDevice:
				return process_manager::GpuUsageStateEnum::NoDevice;
			default:
				return process_manager::GpuUsageStateEnum::Failed; // InitFailed or QueryFailed
			}
		}

		void Log(const gpu_monitor::GpuSnapshot& gpu) const
		{
			switch (state)
			{
			case process_manager::GpuUsageStateEnum::Sampled:
				logger::Info("GPU use is sampled through %s", gpu_monitor::nvml_library);
				break;
			case process_manager::GpuUsageStateEnum::NoDriver:
				logger::Info("GPU use is not reported: %s could not be loaded (no NVIDIA driver)", gpu_monitor::nvml_library);
				break;
			case process_manager::GpuUsageStateEnum::NoDevice:
				logger::Info("GPU use is not reported: NVML finds no GPU");
				break;
			case process_manager::GpuUsageStateEnum::Failed:
				logger::Error("GPU use is not reported: NVML failed with error %d", gpu.nvmlError);
				break;
			case process_manager::GpuUsageStateEnum::Stalled:
				logger::Error("GPU use is not reported: no GPU sample for %lld ms, a driver call is not returning",
					static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - gpu.takenAt).count()));
				break;
			default:
				break;
			}
		}

		process_manager::GpuUsageStateEnum state = process_manager::GpuUsageStateEnum::NotSampled;
		process_manager::GpuUsageStateEnum loggedState = process_manager::GpuUsageStateEnum::NotSampled;
		std::chrono::steady_clock::time_point takenAt{};
		std::vector<GpuProcess> processes;
	};
}

int ProcessManager::StartUp()
{
	process_manager::ProcessGroupBuilder::GetInstance().GenerateGroups();

	auto cycleDetected = !SortByDependencies(errorList, processParams->processList);
	CheckProcessConfig(errorList);

	if (!errorList.empty())
	{

		logger::Error("================================================");
		logger::Error("PLEASE FIX MENTIONED WARNINGS IN THE CONFIG FILE");
		for (const auto& error : errorList)
		{
			logger::Error("%s", error.c_str());
		}
		logger::Error("================================================");

	}
	if (cycleDetected)
	{
		logger::Error("!!!PROCESS MANAGER CANNOT START WITH CYCLE DEPENDENCIES!!!");
		return 0;
	}
	return 1;
}

ProcessManager::ProcessManager(std::unique_ptr<ProcessManagerParams> args)
	:
	zmqContext(1),
	listener(zmqContext, zmq::socket_type::dealer),
	statusPublisher(zmqContext, zmq::socket_type::pub),
	simpleHealthPublisher(zmqContext, zmq::socket_type::pub)
{
	listener.set(zmq::sockopt::routing_id, comm::PROCESS_MANAGER_ID);
	listener.connect(comm::COMMAND_SERVER_PORT);

	statusPublisher.bind(comm::HEALTH_PUBLISH_PORT_DETAILED);
	simpleHealthPublisher.bind(comm::HEALTH_PUBLISH_PORT_SIMPLE);

	processParams = std::move(args);
}
ProcessManager::~ProcessManager()
{
	//shutdown
	listener.close();
	statusPublisher.close();
	simpleHealthPublisher.close();
	zmqContext.close();
}
process_manager::SimpleHealthReportStruct process_manager::ProcessManager::GetSimplifiedHealthReport()
{
	//meliksahs beray spesifik olmasi cokkotu
	process_manager::SimpleHealthReportStruct report;
	report.SDR = GetProcessRuntimeState("GGM"); //TODO @meliksahs
	report.GGM = GetProcessRuntimeState("GGMO");
	if (report.SDR == process_manager::RuntimeStateEnum::Running)
	{
		report.GGM = process_manager::RuntimeStateEnum::Running;
	}
	report.YAM = GetProcessRuntimeState("YAM");
	report.TIM = GetProcessRuntimeState("TIM");
	return report;
}

void process_manager::ProcessManager::Run()
{
	logger::Info("Health status will be published from %s", comm::HEALTH_PUBLISH_PORT_DETAILED);
	logger::Info("Simplified Health status will be published from %s", comm::HEALTH_PUBLISH_PORT_SIMPLE);
	logger::Info("Heartbeats will be listened from %s as %s", comm::COMMAND_SERVER_PORT, comm::PROCESS_MANAGER_ID.c_str());
	// The broker learns a dealer's name from what it sends, and this socket otherwise only receives:
	// a message to itself names it in the broker's log. It comes back here and is ignored.
	listener.send(zmq::buffer(comm::PROCESS_MANAGER_ID), zmq::send_flags::sndmore | zmq::send_flags::dontwait);
	listener.send(zmq::str_buffer("hello"), zmq::send_flags::dontwait);
	zmq::pollitem_t items[] = { { listener.handle(), 0, ZMQ_POLLIN, 0} };

	// Sampled on a thread of its own: a driver call that hangs must not hold up the supervision.
	gpu_monitor::GpuMonitor gpuMonitor{ gpu_monitor::LoadNvml, gpu_monitor::default_period, gpu_monitor::default_retry_period };
	GpuUsageReporter gpuUsage;
	// Reads the journal for a monitor GUI on another machine, which cannot run journalctl here. Without it the
	// manager still supervises; only such a GUI loses its logs.
	journal_server::JournalServer journalServer{ comm::JOURNAL_SERVER_PORT, JournalIdentity(),
		std::string(journal_server::default_journalctl), std::string(journal_server::default_systemctl) };
	std::string journalError;
	if (journalServer.Start(journalError) == journal_server::StartCode::Ok)
	{
		logger::Info("Journal reads will be served from %s", comm::JOURNAL_SERVER_PORT);
	}
	else
	{
		logger::Error("Journal server could not bind %s: %s; a GUI on another machine will show no logs", comm::JOURNAL_SERVER_PORT, journalError.c_str());
	}
	std::vector<process_manager::DetailedHealthReportStruct> published;
	auto nextReport = std::chrono::steady_clock::now();
	while (enabled)
	{
		StartSequentialAsync();

		gpuUsage.Update(gpuMonitor.Latest());
		std::vector<process_manager::DetailedHealthReportStruct> status;
		for (auto& process : processParams->processList)
		{
			process.CheckHeartbeat();
			process.AdvanceStop();
			process.ReapExit();
			process.DrainStdioPipe();
			status.emplace_back(process.GetHealthReport());
			status.back().gpuUsage = gpuUsage.GetUsage(process);
		}
		for (auto& process : retiring)
		{
			process.AdvanceStop();
			process.ReapExit();
			process.DrainStdioPipe();
		}
		retiring.erase(std::remove_if(retiring.begin(), retiring.end(),
			[](const Process& p) { return !p.HasPid(); }), retiring.end());

		const auto now = std::chrono::steady_clock::now();
		if (now >= nextReport || !SameStates(status, published))
		{
			zmq::message_t detailedReport(status.data(), sizeof(process_manager::DetailedHealthReportStruct) * status.size());
			statusPublisher.send(detailedReport, zmq::send_flags::none);

			auto report = GetSimplifiedHealthReport();
			zmq::message_t simplifiedReport(reinterpret_cast<char*>(&report), sizeof(report));
			//listener.send(zmq::buffer(comm::GUI_ID), zmq::send_flags::sndmore);
			simpleHealthPublisher.send(simplifiedReport, zmq::send_flags::none);

			published = std::move(status);
			nextReport = now + report_period;
		}

		// Pipe output is drained once per pass above, so a pass comes at least every 500 ms. The wait also ends
		// when the next report is due.
		const auto longest = IsStopInProgress() ? std::chrono::milliseconds(50) : std::chrono::milliseconds(500);
		const auto untilReport = std::chrono::ceil<std::chrono::milliseconds>(nextReport - std::chrono::steady_clock::now());
		const auto timeout = std::clamp(untilReport, std::chrono::milliseconds(0), longest);
		try
		{
			zmq::poll(items, 1, timeout);
		}
		catch (const zmq::error_t& e)
		{
			if (e.num() != EINTR) throw;
		}

		// Take every queued message, so a backlog never makes a healthy process look silent.
		for (int i = 0; i < max_messages_per_cycle; ++i)
		{
			std::vector<zmq::message_t> messages;
			try
			{
				if (!zmq::recv_multipart(listener, std::back_inserter(messages), zmq::recv_flags::dontwait)) break;
			}
			catch (const std::exception& e)
			{
				logger::Debug("%s", e.what());
				break;
			}
			ProcessMessage(messages);
		}
	}
	logger::Debug("Exiting");
	StopAll(); // here, on the thread that supervises the processes

	return;
}
void process_manager::ProcessManager::RequestStop()
{
	enabled = false;
}
int process_manager::ProcessManager::TearDown()
{
	enabled = false;
	logger::Debug("TearDown");
	StopAll();
	return 0;
}
// Blocking, for shutdown: every process gets SIGTERM at once, then all are waited for together.
void process_manager::ProcessManager::StopAll()
{
	for (auto& process : processParams->processList)
	{
		process.SuspendHealthCheck();
		process.Stop();
	}
	for (auto& process : retiring)
	{
		process.Stop();
	}
	while (IsStopInProgress())
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		for (auto& process : processParams->processList) process.AdvanceStop();
		for (auto& process : retiring) process.AdvanceStop();
	}
}
void process_manager::ProcessManager::RetireProcess(Process& process)
{
	process.SuspendHealthCheck();
	process.Stop();
	if (process.HasPid()) retiring.push_back(std::move(process));
}
bool process_manager::ProcessManager::IsStopInProgress() const
{
	for (const auto& process : processParams->processList)
	{
		if (process.IsStopping()) return true;
	}
	for (const auto& process : retiring)
	{
		if (process.IsStopping()) return true;
	}
	return false;
}
std::unique_ptr<ProcessManagerParams> process_manager::ProcessManager::GenerateConfig(const config_section::ConfigValue& document, std::vector<std::string>* errors)
{
	auto processManagerParams = std::make_unique<ProcessManagerParams>();
	processManagerParams->document = document;
	std::vector<std::string> problems;
	const auto addProblems = [&problems](const std::vector<std::string>& found) { problems.insert(problems.end(), found.begin(), found.end()); };

	config_section::ConfigSection top(document, "the configuration");
	bool hasDefaultWorkMode = false;
	if (const config_section::ConfigValue* managerValue = top.ReadSection("ProcessManager"))
	{
		config_section::ConfigSection manager(*managerValue, "ProcessManager");
		// Inherit: no default, so a service without a WorkMode of its own is Manual.
		const int mode = manager.ReadChoice("Mode", { "Inherit", "Manual", "Auto" }, 0);
		hasDefaultWorkMode = mode != 0;
		processManagerParams->workMode = mode == 2 ? WorkModeEnum::Auto : WorkModeEnum::Manual;
		// The choices in logger::Level order.
		processManagerParams->logLevel = static_cast<logger::Level>(manager.ReadChoice("LogLevel", { "Verbose", "Debug", "Info", "Warning", "Error" }, 1));
		addProblems(manager.Problems());
	}
	const config_section::ConfigValue* services = top.ReadSequence("Services");
	addProblems(top.Problems());

	for (size_t i = 0; services != nullptr && i < services->children.size(); i++)
	{
		const config_section::ConfigValue& item = services->children[i];
		const config_section::ConfigValue* name = config_section::FindValue(item, "ProcessName");
		const std::string label = name != nullptr && !name->text.empty() ? "service '" + name->text + "'" : "service " + std::to_string(i + 1);
		config_section::ConfigSection section(item, label);
		if (!section.ReadBool("Enabled", true)) continue; // switched off: nothing else of it is read or checked

		Process process;
		const int loaded = process.UpdateArgs(section);
		if (loaded == -2)
		{
			problems.push_back(label + ": ProcessName is reserved (pm, all, BPM, PMC, GUI) or contains '/'; service ignored");
			continue;
		}
		if (loaded != 1)
		{
			problems.push_back(label + ": ProcessName and ExecutablePath are required; service ignored");
			continue;
		}
		addProblems(section.Problems());
		// [ProcessManager] Mode applies to every service without a WorkMode of its own.
		if (hasDefaultWorkMode && !section.Has("WorkMode"))
		{
			process.SetWorkMode(processManagerParams->workMode);
		}
		processManagerParams->processList.emplace_back(std::move(process));
	}

	for (const auto& problem : problems)
	{
		if (errors) errors->push_back(problem);
		else logger::Error("%s", problem.c_str());
	}

	return processManagerParams;
}
bool process_manager::ProcessManager::SortByDependencies(std::vector<std::string>& errors, std::vector<process_manager::Process>& pList)
{
	std::unordered_map<std::string, size_t> idx;

	idx.reserve(pList.size());
	for (size_t i = 0; i < pList.size(); ++i)
	{
		if (idx.find(pList[i].GetProcessName()) != idx.end())
		{
			errors.push_back("duplicate process name: " + pList[i].GetProcessName());
			continue;
		}
		idx[pList[i].GetProcessName()] = i;
	}

	//Kahn's Algorithm
	// Build graph: dep -> dependents
	std::vector<std::vector<size_t>> adj(pList.size());
	std::vector<int> indeg(pList.size(), 0);

	for (size_t i = 0; i < pList.size(); ++i)
	{
		for (const auto& dep : pList[i].GetDependencyList())
		{
			auto it = idx.find(dep);
			if (it == idx.end())
			{
				errors.push_back("missing dependency: " + pList[i].GetProcessName() + " depends on '" + dep + "'");
				continue; // ignore unknown deps so we can still order what we know
			}
			adj[it->second].push_back(i); // dep -> i
			indeg[i] += 1;
		}
	}

	std::queue<size_t> q;
	for (size_t i = 0; i < pList.size(); ++i)
	{
		if (indeg[i] == 0) q.push(i);
	}

	std::vector<size_t> order;
	order.reserve(pList.size());

	while (!q.empty())
	{
		size_t u = q.front(); q.pop();
		order.push_back(u);
		for (size_t v : adj[u])
		{
			if (--indeg[v] == 0) q.push(v);
		}
	}

	// Cycle detection
	if (order.size() != pList.size())
	{
		std::string cyc = "cycle detected among: ";
		bool first = true;
		for (size_t i = 0; i < pList.size(); ++i)
		{
			if (indeg[i] > 0)
			{
				if (!first) cyc += ", ";
				cyc += pList[i].GetProcessName();
				first = false;
			}
		}
		errors.push_back(cyc);
	}

	std::vector<Process> out;
	out.reserve(pList.size());

	std::vector<char> used(pList.size(), 0);
	for (size_t i : order) { out.push_back(std::move(pList[i])); used[i] = 1; }
	for (size_t i = 0; i < pList.size(); ++i)
	{
		if (!used[i]) out.push_back(std::move(pList[i]));
	}


	pList = std::move(out);

	return order.size() == pList.size(); // true => no cycles

}

bool process_manager::ProcessManager::CheckProcessConfig(std::vector<std::string>& err) const
{
	for (const auto& process : processParams->processList)
	{
		process.CheckConfig(err);
	}
	return false;
}

bool process_manager::ProcessManager::ApplyToAll(ProcessManagerCommandEnum command)
{
	if (command == ProcessManagerCommandEnum::StopService)
	{
		for (auto& process : processParams->processList)
		{
			process.SetWorkMode(WorkModeEnum::Manual);
			process.SuspendHealthCheck();
			process.Stop();
		}
		return true;
	}
	if (command == ProcessManagerCommandEnum::StartService || command == ProcessManagerCommandEnum::RestartService)
	{
		logger::Debug("--start all received, resetting config");
		for (auto& process : processParams->processList)
		{
			RetireProcess(process); // the new instances start once these have exited
		}
		processParams = GenerateConfig(processParams->document); //reset the runtime params

		std::vector<std::string> errors;
		SortByDependencies(errors, processParams->processList);
		for (const auto& error : errors)
		{
			logger::Error("%s", error.c_str());
		}
		for (auto& process : processParams->processList)
		{
			process.SetWorkMode(WorkModeEnum::Auto);
		}
		return true;
	}
	return false;
}

void process_manager::ProcessManager::StartDependencies(const Process& process) const
{
	for (const auto& dependsOn : process.GetDependencyList())
	{
		auto dependency = GetProcess(dependsOn);
		if (dependency == processParams->processList.end())
		{
			logger::Error("[%s] Cannot start missing dependency %s", process.GetProcessName().c_str(), dependsOn.c_str());
			continue;
		}
		dependency->StartProcess(); // no-op while it runs
	}
}

bool process_manager::ProcessManager::ApplyCommandServerMessage(const ProcessManagerCommandStruct& message)
{
	logger::Debug("Command received");
	const auto& command = message.command;
	const std::string processName = FixedString(message.ServiceName, sizeof(message.ServiceName));
	if (processName == "all")
	{
		return ApplyToAll(command);
	}
	auto it = GetProcess(processName);
	if (it == processParams->processList.end())
	{
		logger::Warning("Cannot find %s", processName.c_str());
		return false;
	}
	else if (command == ProcessManagerCommandEnum::StartService)
	{
		StartDependencies(*it);
		it->SetWorkMode(WorkModeEnum::Auto);
		if (it->IsStopping())
		{
			it->Restart(); // starts again once the stop in progress finishes
		}
		else
		{
			it->StartProcess(); // no-op while it runs
		}
	}
	else if (command == ProcessManagerCommandEnum::StartServiceWithArgs)
	{
		if (it->HasPid())
		{
			logger::Error("[%s] Already running, stop it before starting it with other arguments", processName.c_str());
			return false;
		}
		const std::string argsText = FixedString(message.Args, sizeof(message.Args));
		it->OverrideStartupArgs(config_section::SplitList(argsText, ' '));

		it->StartProcess();
		it->SetWorkMode(WorkModeEnum::Auto);
	}
	else if (command == ProcessManagerCommandEnum::StopService)
	{
		it->SetWorkMode(WorkModeEnum::Manual);
		it->SuspendHealthCheck();
		it->Stop();
	}
	else if (command == ProcessManagerCommandEnum::RestartService)
	{
		StartDependencies(*it);
		it->SetWorkMode(WorkModeEnum::Auto);
		if (it->HasPid())
		{
			it->Restart();
		}
		else
		{
			it->StartProcess();
		}
	}
	else if (command == ProcessManagerCommandEnum::DisableService)
	{
		RetireProcess(*it);
		processParams->processList.erase(it);
	}
	else
	{
		return false;
	}
	return true;
}

bool process_manager::ProcessManager::ProcessMessage(const std::vector<zmq::message_t>& messages)
{
	if (messages.size() < 2) return false;

	const auto& receivedFrom = messages[0].to_string_view();
	if (receivedFrom == comm::COMMAND_SERVER_ID ||
		receivedFrom == comm::GUI_ID)
	{
		const auto& payload = messages[1];
		if (payload.size() == 0) return false;
		const auto command = static_cast<ProcessManagerCommandEnum>(*static_cast<const char*>(payload.data()));
		if (command == ProcessManagerCommandEnum::GetVersionInfo)
		{
			VersionReportStruct version{};
			version.command = ProcessManagerCommandEnum::GetVersionInfo;
#ifdef GITV_PROJECT_VERSION_MAJOR
			version.reserved[0] = GITV_PROJECT_VERSION_GUI;
			version.syy.major = GITV_PROJECT_VERSION_MAJOR;
			version.syy.minor = GITV_PROJECT_VERSION_MINOR;
			version.syy.patch = GITV_PROJECT_VERSION_PATCH;
			version.dam.major = GITV_PROJECT_VERSION_MAJOR;
			version.dam.minor = GITV_PROJECT_VERSION_MINOR;
			version.dam.patch = GITV_PROJECT_VERSION_PATCH;
			version.dam2.major = GITV_PROJECT_VERSION_MAJOR;
			version.dam2.minor = GITV_PROJECT_VERSION_MINOR;
			version.dam2.patch = GITV_PROJECT_VERSION_PATCH;
			version.bpm.major = GITV_PROJECT_VERSION_MAJOR;
			version.bpm.minor = GITV_PROJECT_VERSION_MINOR;
			version.bpm.patch = GITV_PROJECT_VERSION_PATCH;
			version.tim.major = GITV_PROJECT_VERSION_MAJOR;
			version.tim.minor = GITV_PROJECT_VERSION_MINOR;
			version.tim.patch = GITV_PROJECT_VERSION_PATCH;
#endif // GITV_PROJECT_VERSION_MAJOR

#ifdef GITV_YAM_VERSION_MAJOR
			version.yam.major = GITV_YAM_VERSION_MAJOR;
			version.yam.minor = GITV_YAM_VERSION_MINOR;
			version.yam.patch = GITV_YAM_VERSION_PATCH;
#endif // GITV_YAM_VERSION_MAJOR

#ifdef GITV_GSMGGM_VERSION_MAJOR
			version.ggm.major = GITV_GSMGGM_VERSION_MAJOR;
			version.ggm.minor = GITV_GSMGGM_VERSION_MINOR;
			version.ggm.patch = GITV_GSMGGM_VERSION_PATCH;
			version.gsm.major = GITV_GSMGGM_VERSION_MAJOR;
			version.gsm.minor = GITV_GSMGGM_VERSION_MINOR;
			version.gsm.patch = GITV_GSMGGM_VERSION_PATCH;
#endif // GITV_GSMGGM_VERSION_MAJOR


			auto result = reinterpret_cast<char*>(&version);
			listener.send(zmq::buffer(comm::GUI_ID), zmq::send_flags::sndmore);
			listener.send(zmq::buffer(result, sizeof(VersionReportStruct)), zmq::send_flags::none);
			return true;
		}
		else
		{
			// A short frame leaves the rest zero; a longer one is cut to the struct.
			ProcessManagerCommandStruct message{};
			std::memcpy(&message, payload.data(), std::min(payload.size(), sizeof(message)));
			auto reply = ApplyCommandServerMessage(message);
			auto resultString = std::to_string(reply);
			if (messages[0].to_string_view() == comm::GUI_ID)
			{
				listener.send(zmq::buffer(comm::GUI_ID), zmq::send_flags::sndmore);
				listener.send(zmq::buffer(resultString, 1), zmq::send_flags::none);
			}
			return true;

		}
	}
	else
	{
		if (auto it = GetProcess(receivedFrom); it != processParams->processList.end())
		{
			it->HeartbeatReceived();
			return true;
		}
		return false;
	}
}

bool process_manager::ProcessManager::StartSequentialAsync() const
{
	for (const auto& process : retiring)
	{
		if (process.IsStopping()) return false; // replaced instances exit before anything new starts
	}
	for (auto& process : processParams->processList)
	{
		if (process.IsRestartRequested() && !process.HasPid() && !process.IsStopping())
		{
			process.StartProcess();
			return false;
		}
		if (process.IsWorkingManually()) continue;
		if (process.IsStopping()) return false;
		if (process.IsGracePeriod()) break;
		if (process.HasPid())
		{
			if (!process.NeedsRestart()) continue;
			process.StopUnresponsive(); // the restart policy starts it again once it has exited
			return false;
		}
		if (process.NeedsStart())
		{
			process.StartProcess();
			return false;
		}
		if (process.NeedsRestart())
		{
			process.AutoRestart();
			return false;
		}


	}
	return true;
}

std::vector<Process>::iterator process_manager::ProcessManager::GetProcess(std::string_view processName) const
{
	const auto& listStart = processParams->processList.begin();
	const auto& listEnd = processParams->processList.end();

	auto it = std::find_if(listStart, listEnd,
		[&](const Process& p)
		{
			return p.GetProcessName() == processName;
		});

	return it;
}

process_manager::RuntimeStateEnum process_manager::ProcessManager::GetProcessRuntimeState(std::string_view processName) const
{
	if (auto it = GetProcess(processName); it != processParams->processList.end())
	{
		return it->GetRuntimeState();
	}
	else return process_manager::RuntimeStateEnum::Unknown;
}
