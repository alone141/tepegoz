#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <queue>

#include <ProcessManager.h>
#include <ConfigHelper.h>
#include <PipeStruct.h>

#include <zmq_addon.hpp>

#include <legacy/common/logger/Log.h>

#include <algorithm>

#include <ProcessGroupBuilder.h>
using namespace process_manager;

int ProcessManager::StartUp()
{
	process_manager::ProcessGroupBuilder::GetInstance().GenerateGroups();

	auto cycleDetected = !SortByDependencies(errorList, processParams->processList);
	CheckProcessConfig(errorList);

	if (!errorList.empty())
	{

		LOGF_ERROR("================================================");
		LOGF_ERROR("PLEASE FIX MENTIONED WARNINGS IN THE CONFIG FILE");
		for (const auto& error : errorList)
		{
			LOGF_ERROR("%s", error.c_str());
		}
		LOGF_ERROR("================================================");

	}
	if (cycleDetected)
	{
		LOGF_ERROR("!!!PROCESS MANAGER CANNOT START WITH CYCLE DEPENDENCIES!!!");
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
	LOGF_INFO("Health status will be published from %s", comm::HEALTH_PUBLISH_PORT_DETAILED);
	LOGF_INFO("Simplified Health status will be published from %s", comm::HEALTH_PUBLISH_PORT_SIMPLE);
	LOGF_INFO("Heartbeats will be listened from %s as %s", comm::COMMAND_SERVER_PORT, comm::PROCESS_MANAGER_ID.c_str());
	zmq::pollitem_t items[] = { { listener.handle(), 0, ZMQ_POLLIN, 0} };

	while (enabled)
	{
		StartSequentialAsync();

		std::vector<process_manager::DetailedHealthReportStruct> status;
		for (auto& process : processParams->processList)
		{
			process.CheckHeartbeat();
			process.ReapExit();
			status.emplace_back(process.GetHealthReport());
		}
		zmq::message_t detailedReport(status.data(), sizeof(process_manager::DetailedHealthReportStruct) * status.size());
		statusPublisher.send(detailedReport, zmq::send_flags::none);

		auto report = GetSimplifiedHealthReport();
		zmq::message_t simplifiedReport(reinterpret_cast<char*>(&report), sizeof(report));
		//listener.send(zmq::buffer(comm::GUI_ID), zmq::send_flags::sndmore);
		simpleHealthPublisher.send(simplifiedReport, zmq::send_flags::none);

		zmq::poll(items, 1, std::chrono::milliseconds(500));
		const auto messageReceived = items[0].revents & ZMQ_POLLIN;


		if (!messageReceived) continue;

		std::vector<zmq::message_t> messages;

		try
		{
			(void)zmq::recv_multipart(listener, std::back_inserter(messages));
		}
		catch (const std::exception& e) { LOGF_DEBUG("%s", e.what()); }

		bool result = ProcessMessage(messages);
	}
	LOGF_DEBUG("Exiting");

	return;
}
int process_manager::ProcessManager::TearDown()
{
	enabled = false;
	LOGF_DEBUG("TearDown");
	StopAll();
	return 0;
}
legacy::object::ObjectType process_manager::ProcessManager::GetType() const
{
	return legacy::object::ProcessManager;
}
void process_manager::ProcessManager::StopAll()
{
	for (auto& process : processParams->processList)
	{
		process.DisableHealthCheck();
		process.RequestStopAndEscalate(std::chrono::milliseconds(500));
	}
}
std::unique_ptr<ProcessManagerParams> process_manager::ProcessManager::GenerateConfig()
{
	auto processManagerParams = std::make_unique<ProcessManagerParams>();

	Process process;
	char section[] = "0";
	for (size_t i = 0; i < 10; i++)
	{
		section[0]++;
		if (process.UpdateArgs(section) != 1) continue;
		processManagerParams->processList.emplace_back(process);
	}

	const char* temp;
	ConfigHelper::ReadConfiguration("ProcessManager", "Mode", temp, "Inherit");
	if (!strcmp(temp, "Manual")) { processManagerParams->workMode = WorkModeEnum::Manual; }
	else if (!strcmp(temp, "Auto")) { processManagerParams->workMode = WorkModeEnum::Auto; }

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

	std::vector<char> used(pList.size(), 0);
	for (size_t i : order) { out.push_back(pList[i]); used[i] = 1; }
	for (size_t i = 0; i < pList.size(); ++i)
	{
		if (!used[i]) out.push_back(pList[i]);
	}


	pList = out;

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

bool process_manager::ProcessManager::ApplyCommandServerMessage(const ProcessManagerCommandStruct& message)
{
	LOGF_DEBUG("Command received");
	const auto& command = message.command;
	const auto& processName = message.ServiceName;
	if (processName == "all")
	{
		if (command == ProcessManagerCommandEnum::StopService)
		{
			processParams->workMode = WorkModeEnum::Manual;
			StopAll();
		}
		else if (command == ProcessManagerCommandEnum::StartService)
		{
			StopAll();
			LOGF_DEBUG("--start all received, resetting config");
			processParams = GenerateConfig(); //reset the runtime params 
			processParams->workMode = WorkModeEnum::Auto;
		}
		return true;
	}
	auto it = GetProcess(processName);
	if (it == processParams->processList.end())
	{
		std::cout << "Cannot find " << processName << std::endl;
		return false;
	}
	else if (command == ProcessManagerCommandEnum::StartService)
	{
		for (auto& depensOn : it->GetDependencyList())
		{
			GetProcess(depensOn)->StartProcess();
		}
		it->StartProcess();
		it->SetWorkMode(WorkModeEnum::Auto);
	}
	else if (command == ProcessManagerCommandEnum::StartServiceWithArgs)
	{
		std::vector<std::string> args;
		ConfigHelper::SplitElementsToVector(message.Args, ' ', args);
		it->OverrideStartupArgs(args);

		it->StartProcess();
		it->SetWorkMode(WorkModeEnum::Auto);
	}
	else if (command == ProcessManagerCommandEnum::StopService)
	{
		it->SetWorkMode(WorkModeEnum::Manual);
		it->DisableHealthCheck();
		it->RequestStopAndEscalate();
	}
	else if (command == ProcessManagerCommandEnum::DisableService)
	{
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
		//meliksahs TODO cirkin oldu
		auto command = reinterpret_cast<const ProcessManagerCommandEnum*>(messages[1].data())[0];
		if (command == ProcessManagerCommandEnum::GetVersionInfo)
		{
			VersionReportStruct version;
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
			ProcessManagerCommandStruct message;
			message = *reinterpret_cast<const ProcessManagerCommandStruct*>(messages[1].data());
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
	for (auto& process : processParams->processList)
	{
		if (process.IsWorkingManually()) continue;
		if (process.IsGracePeriod()) break;
		if (process.HasPid() && !process.NeedsRestart()) continue;
		if (process.IsStarting() || process.NeedsRestart() || process.NeedsStart())
		{
			process.RequestStopAndEscalate(std::chrono::milliseconds(0));
			process.StartProcess();
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
