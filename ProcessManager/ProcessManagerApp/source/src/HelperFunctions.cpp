#include <HelperFunctions.h>
#include <iostream>
#include <string_view>
#include <cxxopts.hpp>
#include <ProcessManagerApp.h>

const char* ConfigTemplate =
"\n\
; Auto: Automatically start all registered services at the program start \n\
; Manual: Wait for a start signal --start \n\
[ProcessManager] \n\
Mode = Auto \n\
 \n\
 \n\
; We can have services numbered from[1] - [9] \n\
; Each service MUST have a ProcessName and ExecutablePath \n\
; All other parameters are optional \n\
; If you want to not ignore a service change its number to 'x' \n\
; Eg[3] ->[x] Now[x] is ignored and will not be included in any conditions \n\
 \n\
 \n\
[1] \n\
; MUST: Process name used for logging and on the DependsOn list \n\
ProcessName = keepalive \n\
; MUST: Path of the Executable.Must be absolute path \n\
ExecutablePath = / opt / app / bin / keepalive \n\
; OPTIONAL: Program argumets. Must be seperated by empty space' ' \n\
; EXAMPLE: --port 1001 --silent \n\
; DEFAULT: Empty \n\
Args = 10 \n\
; OPTIONAL: Directory that the executable will run in \n\
; DEFAULT: Empty \n\
Cwd = / opt / app \n\
; OPTIONAL: Weather the service should have its own cgroup or not \n\
; DEFAULT: True \n\
CreateProcessGroup = True \n\
; UNUSED \n\
InheritParentEnviroment = false \n\
; OPTIONAL: Directory that the executable will run in \n\
; DEFAULT: Inherit \n\
; Inherit: Output terminal will be same as the process manager \n\
; Pipe: Output will be redirected to the process manager \n\
; File: Output will be redirected to a file \n\
; NullDevice: Output will be redirected to void \n\
StdioMode = NullDevice \n\
; OPTIONAL: Only available in StdioMode::File \n\
; DEFAULT: / tmp / output.txt \n\
; Redirected output will be written to this file \n\
StdioFilePath = / opt / app / output.txt \n\
; OPTIONAL: Only available in StdioMode::File \n\
; DEFAULT: false \n\
StdioAppend = false \n\
; OPTIONAL: Waits x ms to call force shutdown \n\
; DEFAULT: 10000 \n\
ForceKillTimeoutMs = 100 \n\
; OPTIONAL: Service will start after all dependencies are met \n\
; DEFAULT: Empty \n\
; Dependencies are consisted of ProcessName seperated By ' ' \n\
; EXAMPLE: sleep ssh \n\
DependsOn = keepalive2 \n\
; OPTIONAL: Service will start after all dependencies are met \n\
; DEFAULT: Empty \n\
; EnviromentVariables, seperated by ';' \n\
EnviromentVariables = PYTHONPATH=/usr/local/lib/python3.10/site-packages \n\
; OPTIONAL: Conditions for restart \n\
; DEFAULT: OnFailure \n\
; Never: Never restart \n\
; OnFailure: Only after exit code is not 0 \n\
; Always: Always restart after any exit \n\
; User: User controls the start ups \n\
RestartMode = Always \n\
; OPTIONAL: Maximum number of restarts \n\
; DEFAULT: 5 \n\
; Restarting is given up  after this count \n\
MaxRestarts = 5 \n\
; OPTIONAL: Does service has a heartbeat mechanism with ZMQ \n\
; DEFAULT: false \n\
; Heartbeats are ZMQ dealer packets send to 'insertportnumber' \n\
; With ZMQ identity same as the ProcessName \n\
CheckHealth = true \n\
; OPTIONAL: After restarts waits x ms to listen to heartbeats \n\
; DEFAULT: 5000 \n\
GracePeriodMs = 20000 \n\
; OPTIONAL: How freqent are the heartbeats \n\
; DEFAULT: 1000 \n\
HeartbeatPeriodMs = 1500 \n\
; OPTIONAL: After missing x heartbeats restart the service \n\
; DEFAULT: 5 \n\
MissedHeartbeatThreshold = 20 \n\
";

int ParseInputFlags(const int& argc, char**& argv)
{
	const std::string prog = (argc > 0 && argv && argv[0]) ? argv[0] : "netmgr";

	cxxopts::Options options(prog, "Process supervisor and message router");

	options.add_options("General")
		("h,help", "Show help and exit")
		("s,start", "Start a service", cxxopts::value<std::string>()->default_value(""))
		("d,disable", "Disable a service", cxxopts::value<std::string>()->default_value(""))
		("k,kill", "Kill a service", cxxopts::value<std::string>()->default_value(""))
		("r,restart", "Restart a service", cxxopts::value<std::string>()->default_value(""))
		("c,config", "Path to config file", cxxopts::value<std::string>()->default_value(""));
	//("ctl-endpoint", "ZMQ control endpoint (ipc://... or tcp://...)", cxxopts::value<std::string>()->default_value(""))
	//("log-level", "trace|debug|info|warn|error", cxxopts::value<std::string>()->default_value("info"))
	//("dry-run", "Parse config and print planned start order; do not spawn", cxxopts::value<bool>()->default_value("false"));

// Positional: command + rest (e.g. start --only A B)
	options.add_options("Commands")
		("command", "Command: validate|graph|stop|status|generate", cxxopts::value<std::string>()->default_value(""));

	options.parse_positional({ "command" }); // first non-option becomes command
	options.positional_help("COMMAND [OPTIONS]");

	// Allow unknown options
	//options.allow_unrecognised_options();

	auto result = options.parse(argc, argv);
	const std::string cmd = result["command"].as<std::string>();
	const std::string startCmd = result["start"].as<std::string>();
	const std::string disableCmd = result["disable"].as<std::string>();
	const std::string killCmd = result["kill"].as<std::string>();
	const std::string restartCmd = result["restart"].as<std::string>();
	const std::string config = result["config"].as<std::string>();

	if (result.count("help"))
	{
		std::cout << options.help({ "General", "Commands" }) << "\n";
		std::cout
			<< "Commands:\n"
			<< "  validate   Validate config (deps exist, no cycles)\n"
			<< "  graph      Print dependency order\n"
			<< "  status     Print current state\n"
			<< "  generate   Generates an example config\n"
			<< "\n";

		return 1;
	}


	if ((cmd == "validate" || cmd == "graph" || cmd == "start"))
	{
		if (config.empty())
		{
			std::cerr << "Command '" << cmd << "' requires --config FILE.\n";
			return 1;
		}
	}


	if (cmd == "status")
	{
		process_manager::ProcessManagerApp::PrintProcessStatus();
		return 1;
	}
	else if (cmd == "generate")
	{
		GenerateTemplateConfig();
		return 1;
	}
	else if (cmd == "validate")
	{
		process_manager::ProcessManagerApp::ValidateConfig(config);
		return 1;
	}
	else if (cmd == "graph")
	{
		process_manager::ProcessManagerApp::PrintDependencyGraph(config);
		return 1;
	}
	else if (!startCmd.empty())
	{
		process_manager::ProcessManagerApp::SendStartService(startCmd);
		return 1;
	}
	else if (!disableCmd.empty())
	{
		process_manager::ProcessManagerApp::SendDisableService(disableCmd);
		return 1;
	}
	else if (!killCmd.empty())
	{
		process_manager::ProcessManagerApp::SendKillService(killCmd);
		return 1;
	}
	else if (!restartCmd.empty())
	{
		process_manager::ProcessManagerApp::SendRestartService(restartCmd);
		return 1;
	}
	return 0;
}
void GenerateTemplateConfig()
{
	std::ofstream fout;

	fout = std::ofstream("ProcessManagerConfig.ini");

	if (fout.is_open())
	{
		fout << ConfigTemplate;
	}

	fout.close();
}