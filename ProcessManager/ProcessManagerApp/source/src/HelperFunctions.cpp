#include <HelperFunctions.h>
#include <fstream>
#include <iostream>
#include <string_view>
#include <cxxopts.hpp>
#include <ProcessManagerApp.h>

const char* ConfigTemplate = R"yaml(# Process manager configuration (YAML)

# Settings of the manager itself.
ProcessManager:
  # Auto: start every service when the manager starts. Manual: wait for --start.
  # A service's own WorkMode overrides this. DEFAULT: Inherit (a service without a WorkMode is Manual)
  Mode: Auto
  # Lowest level that is logged: Verbose, Debug, Info, Warning or Error. DEFAULT: Debug
  LogLevel: Debug

# One entry per service, started in dependency order. Any number of services can be listed.
# Each MUST have a ProcessName and an ExecutablePath; every other key is optional.
# 'validate' and the manager at startup report keys they do not know, such as a misspelled one.
# Put a value in quotes when it has two spaces in a row ("a  b").
Services:
  # MUST: name used for logging, on the DependsOn lists and in commands.
  # pm, all, BPM, PMC and GUI are reserved, and the name cannot contain '/'.
  - ProcessName: keepalive
    # MUST: path of the executable. Must be an absolute path.
    ExecutablePath: /opt/app/bin/keepalive
    # OPTIONAL: false keeps the service in the file but ignores it.
    # DEFAULT: true
    Enabled: true
    # OPTIONAL: program arguments: a list, or one text split at spaces. A list keeps an argument with spaces whole.
    # EXAMPLE: [--port, 1001, --silent]
    # DEFAULT: none
    Args: [10]
    # OPTIONAL: directory the executable runs in.
    # DEFAULT: the manager's
    Cwd: /opt/app
    # OPTIONAL: Auto: started with the manager and restarted by RestartMode. Manual: waits for --start.
    # DEFAULT: ProcessManager Mode, else Manual
    WorkMode: Auto
    # OPTIONAL: whether the service gets a cgroup of its own.
    # DEFAULT: true
    CreateProcessGroup: true
    # OPTIONAL: start with the manager's environment (true) or an empty one (false), plus EnviromentVariables.
    # DEFAULT: true
    InheritParentEnviroment: true
    # OPTIONAL: where stdout and stderr go.
    # DEFAULT: Inherit
    # Inherit: the same output as the process manager (under systemd: a journal stream of its own)
    # Pipe: each line is logged by the process manager
    # File: written to StdioFilePath
    # NullDevice: discarded
    StdioMode: NullDevice
    # OPTIONAL: only with StdioMode File. Its directory must exist.
    # DEFAULT: /tmp/output.txt
    StdioFilePath: /opt/app/output.txt
    # OPTIONAL: only with StdioMode File: append instead of starting the file again.
    # DEFAULT: false
    StdioAppend: false
    # OPTIONAL: waits this many ms after SIGTERM before SIGKILL, at least 1000.
    # DEFAULT: 10000
    ForceKillTimeoutMs: 10000
    # OPTIONAL: services that must be running first, by ProcessName.
    # EXAMPLE: [sleep, ssh]
    # DEFAULT: none
    DependsOn: []
    # OPTIONAL: environment variables, as NAME: value pairs (or a list of NAME=value texts).
    # DEFAULT: none
    EnviromentVariables:
      PYTHONPATH: /usr/local/lib/python3.10/site-packages
    # OPTIONAL: when to restart.
    # DEFAULT: OnFailure
    # Never: never restart
    # OnFailure: after a non-zero exit code, a crash (a signal other than SIGTERM, SIGINT, SIGHUP, SIGPIPE) or missed heartbeats
    # Always: after any exit, and after missed heartbeats
    # UnlessStopped: same as Always
    # In every mode a stop command keeps the service stopped.
    RestartMode: Always
    # OPTIONAL: maximum number of restarts in a row. Restarting is given up after this count;
    # a run of RestartResetMs or longer starts the count again.
    # DEFAULT: 5
    MaxRestarts: 5
    # OPTIONAL: waits this many ms after an exit before the first restart.
    # DEFAULT: 1000
    RestartDelayMs: 1000
    # OPTIONAL: each further restart in a row waits this many times longer, at most 5 minutes.
    # DEFAULT: 2.0
    BackoffMultiplier: 2.0
    # OPTIONAL: a run at least this long (ms) resets the restart count and the delay.
    # DEFAULT: 60000
    RestartResetMs: 60000
    # OPTIONAL: whether the service sends ZMQ heartbeats: DEALER messages to 'BPM' through
    # tcp://localhost:5557, with the ProcessName as ZMQ identity.
    # DEFAULT: false
    CheckHealth: true
    # OPTIONAL: after a start, waits this many ms before heartbeats are checked.
    # DEFAULT: 5000
    GracePeriodMs: 20000
    # OPTIONAL: how often the heartbeats come, in ms.
    # DEFAULT: 1000
    HeartbeatPeriodMs: 1500
    # OPTIONAL: after this many heartbeat periods without one the service is Unhealthy and restarted (per RestartMode).
    # DEFAULT: 5, at least 1
    MissedHeartbeatThreshold: 20
    # OPTIONAL: sets DISPLAY and XAUTHORITY for a graphical service.
    # DEFAULT: false
    UseDisplay: false
    # OPTIONAL: only with UseDisplay.
    # DEFAULT: ":1"
    Display: ":1"
    # OPTIONAL: only with UseDisplay.
    # DEFAULT: /home/user/.Xauthority
    XAuthority: /home/user/.Xauthority
)yaml";

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
		("c,config", "Path to the YAML config file", cxxopts::value<std::string>()->default_value(""));
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

	// cxxopts throws for a command line it cannot parse, such as an unknown option or one without its value.
	cxxopts::ParseResult result;
	try
	{
		result = options.parse(argc, argv);
	}
	catch (const cxxopts::exceptions::exception& e)
	{
		std::cerr << e.what() << ". Use --help to list the options.\n";
		return 1;
	}
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
			<< "  validate   Validate config (keys, values, deps exist, no cycles)\n"
			<< "  graph      Print dependency order\n"
			<< "  status     Print current state\n"
			<< "  generate   Generates an example config (ProcessManagerConfig.yaml)\n"
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

	fout = std::ofstream("ProcessManagerConfig.yaml");

	if (fout.is_open())
	{
		fout << ConfigTemplate;
	}

	fout.close();
}