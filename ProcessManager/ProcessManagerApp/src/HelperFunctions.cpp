#include "HelperFunctions.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>
#include <cxxopts.hpp>
#include "ProcessManagerApp.hpp"
#include "CommandWatch.hpp"
#include "RegistryClient.hpp"
#include "StatusClient.hpp"

namespace process_manager
{

const char* config_template = R"yaml(# Tepegoz configuration (YAML)
# More services can be registered one per file in the services.d folder next to this file: <ProcessName>.yaml,
# holding the keys of one Services entry below. They come after the services listed here, in file name order.

# Settings of the manager itself.
Tepegoz:
  # Auto: start every service when the manager starts. Manual: wait for --start.
  # A service's own WorkMode overrides this. DEFAULT: Inherit (a service without a WorkMode is Manual)
  Mode: Auto
  # Lowest level that is logged: Verbose, Debug, Info, Warning or Error. DEFAULT: Debug
  LogLevel: Debug
  # Where services can be registered from, through port 6671: false, this machine only; true, any machine that
  # reaches the port. The port has no authentication and the manager runs as root, so whoever can send to it can
  # make the manager run any program. Read at start-up only. DEFAULT: false
  RemoteRegistration: false

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
    # DEFAULT: Tepegoz Mode, else Manual
    WorkMode: Auto
    # OPTIONAL: whether the service gets a cgroup of its own.
    # DEFAULT: true
    CreateProcessGroup: true
    # OPTIONAL: start with the manager's environment (true) or an empty one (false), plus EnvironmentVariables.
    # The old spelling InheritParentEnviroment still works, and 'validate' reports it.
    # DEFAULT: true
    InheritParentEnvironment: true
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
    # The old spelling EnviromentVariables still works, and 'validate' reports it.
    # DEFAULT: none
    EnvironmentVariables:
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

namespace
{

// The manager on this machine: its health reports and its broker.
status_client::Endpoints LocalManager()
{
    status_client::Endpoints endpoints{};
    endpoints.reports = comm::health_listen_port_detailed;
    endpoints.commands = comm::command_server_port;
    return endpoints;
}

int SendCommand(command_watch::Command command, const std::string& serviceName, bool follow)
{
    const status_client::CommandCode code = status_client::SendCommand(
        command, serviceName, LocalManager(), follow, status_client::default_command_timeout);
    return status_client::ExitCode(code);
}

void PrintHelp(const cxxopts::Options& options)
{
    std::cout << options.help({ "General", "Commands" }) << "\n";
    std::cout
        << "Commands:\n"
        << "  validate   Validate config (keys, values, deps exist, no cycles)\n"
        << "  graph      Print dependency order\n"
        << "  status     Live table of every service, redrawn each second; --once prints it once\n"
        << "  generate   Generates an example config (tepegoz.yaml)\n"
        << "  reload     The running manager reads its configuration again: it adds new services, removes\n"
        << "             deleted ones, and a changed one takes its new definition at its next start\n"
        << "\n"
        << "Registering services (the running manager writes each one to a file in services.d):\n"
        << "  add NAME EXECUTABLE [ARG...]  Register a service and start it. Words of the program that start\n"
        << "                                with '-' go after '--': add camera /opt/cam/bin/server -- --port 8080\n"
        << "  add NAME --from FILE          Register a service from a YAML file with the keys of one service\n"
        << "  set NAME Key=Value...         Change keys of a registered service; --unset Key removes one\n"
        << "  replace NAME --from FILE      Give a registered service a whole new definition\n"
        << "  remove NAME                   Remove a registered service; it stops\n"
        << "  show NAME                     Print a service's definition as YAML; where it is defined and how\n"
        << "                                it runs go to standard error\n"
        << "  list                          A table of every service: name, origin, state, pid, mark, file\n"
        << "\n"
        << "  --set Key=Value   add: one more key; can be repeated\n"
        << "  --no-start        add: register the service, but do not start it\n"
        << "  --unset Key       set: remove a key; can be repeated\n"
        << "  --restart         set, replace: a running service restarts with its new definition at once\n"
        << "  --force           remove: also when other services depend on it\n"
        << "  --dry-run         add, set, replace, remove: check the request and print what would be\n"
        << "                    written, but write nothing\n"
        << "  --json            show, list: print the manager's answer as JSON\n"
        << "  --registry HOST   The manager to ask: HOST, HOST:PORT or tcp://HOST:PORT (default\n"
        << "                    tcp://localhost:6671). Another machine's manager takes requests only with\n"
        << "                    RemoteRegistration: true\n"
        << "\n"
        << "In Key=Value the value is read as YAML: MaxRestarts=3 is a number, CheckHealth=false a boolean\n"
        << "and 'Args=[--port, 9090]' a list. Any other value is the text as typed, such as Display=:1;\n"
        << "quotes make a text of anything: 'Display=\"1\"'. The manager checks a request as validate does\n"
        << "before anything is written, and a request that would add a problem is refused and writes nothing.\n"
        << "\n"
        << "--start, --kill, --restart and --disable follow the reports for up to 10 s and print\n"
        << "what the command did. Exit code 0: done; 1: failed, unknown service or manager not\n"
        << "reachable; 2: not done within 10 s. With --no-wait the command is only sent.\n"
        << "reload waits up to 10 s for the manager's answer and prints what it did. Exit code 0:\n"
        << "applied, also when nothing changed; 1: refused or manager not reachable; 2: no answer.\n"
        << "add, set, replace, remove, show and list wait up to 10 s for the manager's answer. Exit code\n"
        << "0: done; 1: refused, not understood or manager not reachable; 2: no answer.\n"
        << "\n";
}

// A registry verb has options of its own: --restart is a flag there, where elsewhere it names a service.
int RunRegistryVerb(int argc, char** argv, registry_client::Verb verb, const cxxopts::Options& general)
{
    const std::string verbName{registry_client::VerbName(verb)};
    cxxopts::Options options{general.program(), "Registers services through the running manager"};
    options.add_options()
        ("h,help", "Show help and exit")
        ("from", "A YAML file with the keys of one service", cxxopts::value<std::string>())
        ("set", "Key=Value", cxxopts::value<std::vector<std::string>>())
        ("unset", "Key", cxxopts::value<std::vector<std::string>>())
        ("no-start", "Register without starting")
        ("restart", "Restart with the new definition at once")
        ("force", "Remove also when other services depend on it")
        ("dry-run", "Check and print, write nothing")
        ("json", "Print the manager's answer as JSON")
        ("registry", "Where the manager's service registry is", cxxopts::value<std::string>())
        ("command", "The registry command", cxxopts::value<std::string>())
        ("words", "The command's words", cxxopts::value<std::vector<std::string>>());
    options.parse_positional({ "command", "words" });

    cxxopts::ParseResult result{};
    try
    {
        result = options.parse(argc, argv);
    }
    catch (const cxxopts::exceptions::exception& e)
    {
        std::cerr << e.what() << ". Words of the program that start with '-' go after '--', as in: add camera "
                  << "/opt/cam/bin/server -- --port 8080. Use --help to list the options.\n";
        return 1;
    }

    if (result.count("help"))
    {
        PrintHelp(general);
        return 0;
    }

    // The words are read as they were given: cxxopts would split a list value such as Args=[a,b] at its commas.
    registry_client::VerbLine line{};
    line.verb = verb;
    std::string command;
    for (const cxxopts::KeyValue& argument : result.arguments())
    {
        if (argument.key() == "command")
        {
            command = argument.value();
        }
        else if (argument.key() == "words")
        {
            line.words.push_back(argument.value());
        }
        else if (argument.key() == "set")
        {
            line.sets.push_back(argument.value());
        }
        else if (argument.key() == "unset")
        {
            line.unsets.push_back(argument.value());
        }
    }

    if (command != verbName)
    {
        std::cerr << "'" << command << "' before " << verbName << " is not understood: give the command first. "
                  << "Use --help to list the commands.\n";
        return 1;
    }

    line.from = result.count("from") ? result["from"].as<std::string>() : std::string{};
    line.registry = result.count("registry") ? result["registry"].as<std::string>() : std::string{};
    line.noStart = result.count("no-start") > 0;
    line.restart = result.count("restart") > 0;
    line.force = result.count("force") > 0;
    line.dryRun = result.count("dry-run") > 0;
    line.json = result.count("json") > 0;

    // Without a current folder, a relative path stays relative, and the manager's check refuses it.
    std::error_code ignored{};
    const std::filesystem::path folder = std::filesystem::current_path(ignored);
    return registry_client::ExitCode(registry_client::RunVerb(line, folder, registry_client::reply_timeout));
}

} // namespace

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
        ("no-wait", "With --start, --kill, --restart, --disable or reload: send the command and exit at once")
        ("once", "With status: print the table once and exit")
        ("c,config", "Path to the YAML config file", cxxopts::value<std::string>()->default_value(""));
    //("ctl-endpoint", "ZMQ control endpoint (ipc://... or tcp://...)", cxxopts::value<std::string>()->default_value(""))
    //("log-level", "trace|debug|info|warn|error", cxxopts::value<std::string>()->default_value("info"))
    //("dry-run", "Parse config and print planned start order; do not spawn", cxxopts::value<bool>()->default_value("false"));

// Positional: command + rest (e.g. start --only A B)
    options.add_options("Commands")
        ("command", "Command: validate|graph|status|generate|reload|add|set|replace|remove|show|list", cxxopts::value<std::string>()->default_value(""));

    options.parse_positional({ "command" }); // first non-option becomes command
    options.positional_help("COMMAND [OPTIONS]");

    // add, set, replace, remove, show and list take words and options of their own.
    const std::vector<std::string> args(argc > 1 ? argv + 1 : argv, argc > 1 ? argv + argc : argv);
    const std::optional<registry_client::Verb> verb = registry_client::FindVerb(args);
    if (verb.has_value())
    {
        return RunRegistryVerb(argc, argv, *verb, options);
    }

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
        PrintHelp(options);
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
        if (result.count("once"))
        {
            return status_client::PrintStatus(LocalManager()) == status_client::StatusCode::Ok ? 0 : 1;
        }
        status_client::WatchStatus(LocalManager());
        return 0;
    }
    else if (cmd == "generate")
    {
        GenerateTemplateConfig();
        return 0;
    }
    else if (cmd == "validate")
    {
        return ProcessManagerApp::ValidateConfig(config) ? 0 : 1;
    }
    else if (cmd == "graph")
    {
        ProcessManagerApp::PrintDependencyGraph(config);
        return 0;
    }
    else if (cmd == "reload")
    {
        const bool wait = result.count("no-wait") == 0;
        return status_client::ExitCode(status_client::SendReload(LocalManager(), wait, status_client::default_command_timeout));
    }
    else if (!cmd.empty())
    {
        std::cerr << "Unknown command '" << cmd << "'. Use --help to list the commands.\n";
        return 1;
    }
    const bool follow = result.count("no-wait") == 0;
    if (!startCmd.empty())
    {
        return SendCommand(command_watch::Command::Start, startCmd, follow);
    }
    else if (!disableCmd.empty())
    {
        return SendCommand(command_watch::Command::Disable, disableCmd, follow);
    }
    else if (!killCmd.empty())
    {
        return SendCommand(command_watch::Command::Stop, killCmd, follow);
    }
    else if (!restartCmd.empty())
    {
        return SendCommand(command_watch::Command::Restart, restartCmd, follow);
    }
    configPath = config;
    return -1;
}
void GenerateTemplateConfig()
{
    std::ofstream fout;

    fout = std::ofstream("tepegoz.yaml");

    if (fout.is_open())
    {
        fout << config_template;
    }

    fout.close();
}

} // namespace process_manager
