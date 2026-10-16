#include <HelperFunctions.h>
#include <iostream>
#include <string_view>
#include <cxxopts.hpp>
#include <ProcessManagerApp.h>

const char* ConfigTemplate =
"\n\
; Auto: Automatically start all registered services at the program start \n\
; Manual: Wait for a start signal --start \n\
; A service's own WorkMode overrides this \n\
[ProcessManager] \n\
Mode = Auto \n\
 \n\
 \n\
; We can have services numbered from [1] - [9] \n\
; Each service MUST have a ProcessName and ExecutablePath \n\
; All other parameters are optional \n\
; To ignore a service, change its number to 'x' \n\
; Eg [3] -> [x] Now [x] is ignored and will not be included in any conditions \n\
 \n\
 \n\
[1] \n\
; MUST: Process name used for logging and on the DependsOn list \n\
; pm, all, BPM, PMC and GUI are reserved, and the name cannot contain '/' \n\
ProcessName = keepalive \n\
; MUST: Path of the Executable. Must be absolute path \n\
ExecutablePath = /opt/app/bin/keepalive \n\
; OPTIONAL: Program arguments. Must be separated by empty space ' ' \n\
; EXAMPLE: --port 1001 --silent \n\
; DEFAULT: Empty \n\
Args = 10 \n\
; OPTIONAL: Directory that the executable will run in \n\
; DEFAULT: Empty \n\
Cwd = /opt/app \n\
; OPTIONAL: Auto: started with the manager and restarted by RestartMode. Manual: waits for --start \n\
; DEFAULT: [ProcessManager] Mode, else Manual \n\
WorkMode = Auto \n\
; OPTIONAL: Whether the service should have its own cgroup or not \n\
; DEFAULT: True \n\
CreateProcessGroup = True \n\
; OPTIONAL: Start with the manager's environment (true) or an empty one (false), plus EnviromentVariables \n\
; DEFAULT: true \n\
InheritParentEnviroment = true \n\
; OPTIONAL: Where stdout and stderr go \n\
; DEFAULT: Inherit \n\
; Inherit: Output terminal will be same as the process manager (under systemd: a journal stream of its own) \n\
; Pipe: Each line is logged by the process manager \n\
; File: Output will be redirected to a file \n\
; NullDevice: Output will be redirected to void \n\
StdioMode = NullDevice \n\
; OPTIONAL: Only available in StdioMode::File. Its directory must exist \n\
; DEFAULT: /tmp/output.txt \n\
; Redirected output will be written to this file \n\
StdioFilePath = /opt/app/output.txt \n\
; OPTIONAL: Only available in StdioMode::File \n\
; DEFAULT: false \n\
StdioAppend = false \n\
; OPTIONAL: Waits x ms after SIGTERM before SIGKILL, at least 1000 \n\
; DEFAULT: 10000 \n\
ForceKillTimeoutMs = 10000 \n\
; OPTIONAL: Service will start after all dependencies are met \n\
; DEFAULT: Empty \n\
; Dependencies are consisted of ProcessName separated By ' ' \n\
; EXAMPLE: sleep ssh \n\
DependsOn = \n\
; OPTIONAL: Environment variables, separated by ';' \n\
; DEFAULT: Empty \n\
EnviromentVariables = PYTHONPATH=/usr/local/lib/python3.10/site-packages \n\
; OPTIONAL: Conditions for restart \n\
; DEFAULT: OnFailure \n\
; Never: Never restart \n\
; OnFailure: After a non-zero exit code, a crash (a signal other than SIGTERM, SIGINT, SIGHUP, SIGPIPE) or missed heartbeats \n\
; Always: After any exit, and after missed heartbeats \n\
; UnlessStopped: Same as Always \n\
; In every mode a stop command keeps the service stopped \n\
RestartMode = Always \n\
; OPTIONAL: Maximum number of restarts in a row \n\
; DEFAULT: 5 \n\
; Restarting is given up after this count; a run of RestartResetMs or longer starts the count again \n\
MaxRestarts = 5 \n\
; OPTIONAL: Waits x ms after an exit before the first restart \n\
; DEFAULT: 1000 \n\
RestartDelayMs = 1000 \n\
; OPTIONAL: Each further restart in a row waits this many times longer, at most 5 minutes \n\
; DEFAULT: 2.0 \n\
BackoffMultiplier = 2.0 \n\
; OPTIONAL: A run at least this long (ms) resets the restart count and the delay \n\
; DEFAULT: 60000 \n\
RestartResetMs = 60000 \n\
; OPTIONAL: Does service has a heartbeat mechanism with ZMQ \n\
; DEFAULT: false \n\
; Heartbeats are ZMQ dealer messages to 'BPM' through tcp://localhost:5557 \n\
; With ZMQ identity same as the ProcessName \n\
CheckHealth = true \n\
; OPTIONAL: After a start, waits x ms before heartbeats are checked \n\
; DEFAULT: 5000 \n\
GracePeriodMs = 20000 \n\
; OPTIONAL: How freqent are the heartbeats \n\
; DEFAULT: 1000 \n\
HeartbeatPeriodMs = 1500 \n\
; OPTIONAL: After x heartbeat periods without one the service is Unhealthy and restarted (per RestartMode) \n\
; DEFAULT: 5, at least 1 \n\
MissedHeartbeatThreshold = 20 \n\
; OPTIONAL: Sets DISPLAY and XAUTHORITY for a graphical service \n\
; DEFAULT: false \n\
UseDisplay = false \n\
; OPTIONAL: Only with UseDisplay \n\
; DEFAULT: :1 \n\
Display = :1 \n\
; OPTIONAL: Only with UseDisplay \n\
; DEFAULT: /home/user/.Xauthority \n\
XAuthority = /home/user/.Xauthority \n\
";

int ParseInputFlags(const int& argc, char**& argv, std::string& configPath)
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
		("command", "Command: validate|graph|status|generate", cxxopts::value<std::string>()->default_value(""));

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

		return 0;
	}


	if ((cmd == "validate" || cmd == "graph"))
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
		return 0;
	}
	else if (cmd == "generate")
	{
		GenerateTemplateConfig();
		return 0;
	}
	else if (cmd == "validate")
	{
		return process_manager::ProcessManagerApp::ValidateConfig(config) ? 0 : 1;
	}
	else if (cmd == "graph")
	{
		process_manager::ProcessManagerApp::PrintDependencyGraph(config);
		return 0;
	}
	else if (!cmd.empty())
	{
		std::cerr << "Unknown command '" << cmd << "'. Use --help to list the commands.\n";
		return 1;
	}
	else if (!startCmd.empty())
	{
		return process_manager::ProcessManagerApp::SendStartService(startCmd) ? 0 : 1;
	}
	else if (!disableCmd.empty())
	{
		return process_manager::ProcessManagerApp::SendDisableService(disableCmd) ? 0 : 1;
	}
	else if (!killCmd.empty())
	{
		return process_manager::ProcessManagerApp::SendKillService(killCmd) ? 0 : 1;
	}
	else if (!restartCmd.empty())
	{
		return process_manager::ProcessManagerApp::SendRestartService(restartCmd) ? 0 : 1;
	}
	configPath = config;
	return -1;
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