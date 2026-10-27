# Bihter: Process Manager – Usage guide

The complete reference for running the process manager and its monitor GUI. For the short version, see
[QUICKSTART.md](QUICKSTART.md). For how the parts fit together, see [ARCHITECTURE.md](ARCHITECTURE.md).

- [Installing](#installing)
- [Running the manager](#running-the-manager)
- [Command line](#command-line)
- [Configuration](#configuration)
- [How services are run](#how-services-are-run)
- [Monitor GUI](#monitor-gui)
- [Ports and network access](#ports-and-network-access)
- [Testing with TestServices](#testing-with-testservices)

## Installing

```bash
cmake -S ProcessManager -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j
ctest --test-dir build
sudo cmake --install build
```

The build needs CMake 3.20 or newer and GCC 10 or newer. libzmq, cppzmq, cxxopts, fkYAML and GoogleTest come
with the source in `ProcessManager/third_party/`, so nothing is downloaded and the target machine needs no
ZeroMQ library.

`cmake --install` installs:

| Item | Path with prefix `/usr` | Notes |
|---|---|---|
| Manager | `/usr/bin/iltaren-ProcessManager` | |
| Configuration | `/etc/processmanager/ProcessManagerConfig.yaml` | Only if missing. An existing file is kept; if the new one differs, it is put next to it as `ProcessManagerConfig.yaml.new`. |
| Fallback configuration | `/usr/share/processmanager/ProcessManagerConfig.yaml` | Read when the file above is missing. |
| Log directory | `/var/log/processmanager/ProcessManagerLog/` | |
| systemd unit | `/usr/lib/systemd/system/processmanager.service` | Enabled when installed as root on a running systemd. Not started. |
| Monitor GUI | `/usr/share/processmanager/gui/`, launcher `/usr/bin/process-monitor` | |
| Test programs (tests built) | `/usr/bin/keepalive`, `/usr/bin/pmt_*`, `/usr/share/processmanager/test-setup/` | See [Testing with TestServices](#testing-with-testservices). |

The paths are built into the manager and the unit when you configure, so set the prefix then rather than with
`cmake --install --prefix`. These cache variables change the install:

| Variable | Default | Meaning |
|---|---|---|
| `CMAKE_INSTALL_PREFIX` | `/usr/local` | Base of every path below. |
| `PM_CONFIG_DIR` | `<sysconfdir>/processmanager/`: `/etc/processmanager/` for `/usr`, `/usr/local/etc/processmanager/` for `/usr/local` | Where the manager reads `ProcessManagerConfig.yaml`. |
| `PM_LOG_DIR` | `<localstatedir>/log/processmanager/`: `/var/log/processmanager/` for `/usr`, `/usr/local/var/log/processmanager/` for `/usr/local` | Parent of the `ProcessManagerLog` directory. |
| `PM_SHARE_DIR` | `<prefix>/share/processmanager/` | Fallback configuration, GUI and test setup. |
| `PM_BIN_INSTALL_DIR` | `bin` | Where the programs go. |
| `PM_CONFIG_SOURCE` | empty | The configuration file to install. Empty installs a starter with no services. |
| `PM_SYSTEMD_UNIT_DIR` | `<prefix>/lib/systemd/system` | Where the unit goes. |
| `PM_INSTALL_GUI` | `ON` | Installs the GUI and its launcher. |
| `PM_ENABLE_SERVICE` | `ON` | Enables the unit when installing as root on a running systemd. |
| `ENABLE_TEST` | `ON` | Builds the tests and installs the test programs. |

## Running the manager

### As a systemd service

```bash
sudo systemctl start processmanager
sudo systemctl stop processmanager
sudo systemctl restart processmanager          # also how a changed configuration is read
systemctl status processmanager
journalctl -u processmanager -f
```

The installed unit has these settings:
- **`Delegate=yes`:** the manager can create a cgroup for itself and for each service.
- **`KillMode=mixed`:** a stop sends SIGTERM to the manager alone, and the manager stops its services itself.
  Each service gets up to twice its `ForceKillTimeoutMs`.
- **`TimeoutStopSec=180`:** after 180 s, systemd kills whatever is still running.
- **`Restart=on-failure`:** systemd restarts the manager if it exits with an error or crashes. A manager that
  stops because its broker failed exits with 0, so it is not restarted.

### By hand

```bash
iltaren-ProcessManager                               # reads the installed configuration
iltaren-ProcessManager --config ./my.yaml            # reads this file, with no fallback
```

Without `--config`, the manager reads `PM_CONFIG_DIR/ProcessManagerConfig.yaml`. If that file is missing, it
reads `PM_SHARE_DIR/ProcessManagerConfig.yaml`. With `--config`, a missing file is an error.

### Stopping

- **SIGTERM or SIGINT (Ctrl+C):** the manager stops every service, waits until they are gone, and exits with 0.
- **A second signal:** it exits at once, with 1, without waiting.

The manager also stops its services and exits with 0 if its message broker fails, because without the broker no
heartbeat or command can reach it.

### Startup checks

At startup, the manager logs the same problems that `validate` reports. A service with a missing executable
still fails when it is started. A dependency cycle stops the manager from starting at all.

### Logs

The manager writes its log in three places:
- **On the console (stderr).** Under systemd, this goes to the journal with the right priorities. On a terminal,
  warnings are yellow, errors red and debug lines dim; `NO_COLOR` turns the colours off.
- **In files:** `PMLog_<date>_<time>.log` in `PM_LOG_DIR/ProcessManagerLog/`. A file is continued in a new one
  at 100 MB. If the directory cannot be created, the manager logs to the console only.
- **The level:** `LogLevel` in the configuration sets the lowest level that is logged.

Lines about one service start with its name in brackets, for example `[server] pid 1234 exited with code 1`.
Services in `Pipe` mode have their output logged the same way: `[server] <the line it printed>`.

## Command line

`iltaren-ProcessManager [COMMAND] [OPTIONS]` (named `ProcessManagerApp` in a build tree).

| Command or option | What it does |
|---|---|
| *(nothing)* | Runs the manager with the installed configuration. |
| `--config FILE`, `-c FILE` | Runs the manager with `FILE`. Also names the file for `validate` and `graph`. |
| `validate --config FILE` | Checks `FILE`: YAML syntax with line numbers, unknown keys, wrong values, reserved names, missing executables and folders, missing dependencies, duplicates, cycles. Exits 1 if it finds a problem. |
| `graph --config FILE` | Prints the order in which the services start. |
| `generate` | Writes `ProcessManagerConfig.yaml` with every key documented, in the current folder. |
| `status` | Shows a live table, refreshed every second: name, pid, state, missed beats, restarts, CPU and memory in MB. The CPU column, headed `CPU Usage(us)`, shows percent of one core. CPU and memory show 0 unless the service is Running or Starting. Ctrl+C leaves. |
| `--start NAME`, `-s NAME` | Starts `NAME` and the services in its `DependsOn`. Sets it to Auto, so it is restarted by its `RestartMode`. |
| `--kill NAME`, `-k NAME` | Stops `NAME` and sets it to Manual, so it stays stopped. |
| `--restart NAME`, `-r NAME` | Stops `NAME` and starts it again, with its dependencies started first. Sets it to Auto. |
| `--disable NAME`, `-d NAME` | Stops `NAME` and removes it from the manager until the manager restarts. |
| `--kill all` | Stops every service and sets each to Manual. |
| `--start all`, `--restart all` | Stops every service, rebuilds all of them from the configuration read at startup, and starts every one, Manual ones included. Disabled services come back. |
| `--help`, `-h` | Lists the options. |

- **Where commands go:** `--start`, `--kill`, `--restart` and `--disable` go to the manager on the same machine,
  through `tcp://localhost:5557`.
- **Exit codes:** 0 means the command was sent, not that it succeeded. The tool exits with 1 if the manager does
  not take the command within 2 s, or if `NAME` is longer than 31 bytes.
- **Wrong command lines:** an unknown option, an option without its value, or an unknown command exits with 1
  and a message.

## Configuration

The file is YAML with two parts: `ProcessManager` holds the manager's own settings, and `Services` lists the
programs to run.

```yaml
ProcessManager:
  Mode: Auto
  LogLevel: Info

Services:
  - ProcessName: server
    ExecutablePath: /opt/app/bin/server
    # …more keys
  - ProcessName: other
    ExecutablePath: /opt/app/bin/other
```

- **Checks:** unknown keys, values of the wrong kind, and numbers outside their range are reported by
  `validate` and at startup. A number outside its range is clamped to the nearest limit.
- **Spelling:** two keys are spelled as the code reads them: `EnviromentVariables` and `InheritParentEnviroment`.
- **Quoting:** a value with two spaces in a row must be quoted (`"a  b"`), because of the YAML reader in use.

### `ProcessManager`

| Key | Values | Default | Meaning |
|---|---|---|---|
| `Mode` | `Auto`, `Manual`, `Inherit` | `Inherit` | The `WorkMode` of every service that sets none. `Inherit`: such services are Manual. |
| `LogLevel` | `Verbose`, `Debug`, `Info`, `Warning`, `Error` | `Debug` | Lowest level that is logged. `Verbose` adds every message the broker forwards. |

### `Services`

Each entry is one service. `ProcessName` and `ExecutablePath` are required; everything else is optional.

| Key | Default | Meaning |
|---|---|---|
| `ProcessName` | (required) | The service's name, used in commands, logs, `DependsOn` and heartbeats. Not `pm`, `all`, `BPM`, `PMC` or `GUI`, and no `/`. At most 31 bytes, or commands cannot name it. |
| `ExecutablePath` | (required) | Absolute path of the program. |
| `Enabled` | `true` | `false` keeps the entry in the file but ignores it completely. |
| `Args` | none | The arguments. A list (`[--port, 9000]`) keeps an argument with spaces whole; a text (`--port 9000`) is split at spaces. |
| `Cwd` | the manager's | Working directory. |
| `WorkMode` | from `Mode` | `Auto`: started with the manager and restarted by `RestartMode`. `Manual`: waits for a start command. |
| `DependsOn` | none | Services that start before this one, by `ProcessName`. |
| `RestartMode` | `OnFailure` | `Never`, `OnFailure`, `Always` or `UnlessStopped`. See [Restarts](#restarts). |
| `MaxRestarts` | `5` | Restarts in a row before the manager gives up (0–1000). |
| `RestartDelayMs` | `1000` | Wait after an exit before the first restart. |
| `BackoffMultiplier` | `2.0` | Each further restart in a row waits this many times longer (1.0–10.0), at most 5 minutes. |
| `RestartResetMs` | `60000` | A run at least this long starts the count of restarts in a row again. |
| `ForceKillTimeoutMs` | `10000` | Wait after SIGTERM before SIGKILL (at least 1000). |
| `CheckHealth` | `false` | The service sends heartbeats, and missing ones mark it Unhealthy. |
| `GracePeriodMs` | `5000` | After a start, heartbeats are not checked for this long. |
| `HeartbeatPeriodMs` | `1000` | How often the service sends a heartbeat. |
| `MissedHeartbeatThreshold` | `5` | Heartbeat periods without one before the service is Unhealthy (1–1000). |
| `StdioMode` | `Inherit` | Where stdout and stderr go: `Inherit`, `Pipe`, `File` or `NullDevice`. See [Output](#output). |
| `StdioFilePath` | `/tmp/output.txt` | The file, with `StdioMode: File`. Its folder must exist. |
| `StdioAppend` | `false` | With `File`: add to the file instead of starting it again at each start. |
| `InheritParentEnviroment` | `true` | `true`: start with the manager's environment. `false`: start with an empty one. |
| `EnviromentVariables` | none | Variables to set: a mapping (`NAME: value`) or a list of `NAME=value` texts. |
| `UseDisplay` | `false` | Sets `DISPLAY` and `XAUTHORITY`, for a program that opens windows. |
| `Display` | `":1"` | The `DISPLAY` value, with `UseDisplay`. |
| `XAuthority` | `/home/user/.Xauthority` | The `XAUTHORITY` value, with `UseDisplay`. |
| `CreateProcessGroup` | `true` | Gives the service a cgroup of its own, for CPU and memory figures and a complete stop. |

## How services are run

### Starting

1. **Start order:** with `Mode: Auto` (or `WorkMode: Auto`), the manager starts services one at a time, in
   `DependsOn` order. It does not wait for a service to be healthy, but the next one waits while one ahead of it
   in the order is still in its grace period.
2. **Grace periods hold up later services:**
   - The grace period of `GracePeriodMs` applies after every start, with or without `CheckHealth`.
   - While a service is in its grace period, or being stopped, every Auto service after it in the order waits.
     That covers their starts, their automatic restarts and their stops for missed heartbeats.
   - Keep grace periods short for services early in the order.
3. **Child setup:** the program runs in a process group of its own. The child then enters the working directory,
   joins its cgroup `task_<ProcessName>` (with `CreateProcessGroup`), sets up its output and environment, and
   runs the program.
4. **Odd cases:**
   - A working directory that cannot be entered is reported, but the program still starts.
   - A child that cannot join its cgroup exits with code 1.
   - A program that cannot be run exits with code 127.

`--start` and `--restart` also start the services in the `DependsOn` of the named one, but not their own
dependencies. Stopping or losing a service does not stop the services that depend on it.

### States

| State | Meaning |
|---|---|
| Unknown | Never started (for example a Manual service). |
| Starting | Started, with `CheckHealth`, and no heartbeat yet. |
| Running | Running; with `CheckHealth`, its heartbeats arrive. |
| Unhealthy | Running, but its heartbeats stopped. |
| Stopped | Not running. |

### Restarts

| `RestartMode` | Restarted after |
|---|---|
| `Never` | Nothing. |
| `OnFailure` | A failure: an exit code other than 0, a crash by a signal other than SIGTERM, SIGINT, SIGHUP or SIGPIPE, an exit whose status was lost, or a stop for missed heartbeats. |
| `Always`, `UnlessStopped` | Any exit, and missed heartbeats. (The two behave the same.) |

- **Stop commands win:** in every mode, a stop command (`--kill`, the GUI's Stop) keeps the service stopped.
- **Restarts in a row:** the first restart waits `RestartDelayMs`. Each further one waits `BackoffMultiplier`
  times longer, at most 5 minutes. After `MaxRestarts` restarts in a row, the manager logs `Not restarting` and
  leaves the service stopped.
- **Missed heartbeats count too:** stops for missed heartbeats use up the same `MaxRestarts`. Once it is used
  up, an Unhealthy service is no longer stopped: it stays running and Unhealthy.
- **Starting the count again:** a run that lasts `RestartResetMs` or longer resets the count, and so does a start
  command.
- **What the count shows:** the restart count in `status` and the GUI counts every restart, the ones you asked
  for included.

### Stopping

1. **SIGTERM:** the manager sends SIGTERM to the service's whole process group.
2. **SIGKILL:** after `ForceKillTimeoutMs`, it sends SIGKILL to the group and kills whatever is left in the
   service's cgroup.
3. **Giving up:** if processes remain after another `ForceKillTimeoutMs`, it logs `Force shutdown failed`.

A stop is complete when the service's main process has exited and its process group is empty.

A child that left the process group, for example with `setsid()`, is reached only through the cgroup. That child
gets SIGKILL without a SIGTERM first, and without a cgroup it survives the stop.

### Health checks

With `CheckHealth: true`, the service must send heartbeats through the manager's message broker:
- **Socket:** a ZeroMQ DEALER whose identity is the service's `ProcessName`, connected to `tcp://localhost:5557`.
- **Message:** two frames: `BPM`, then anything; the content is ignored.
- **Rate:** one message every `HeartbeatPeriodMs`.

```cpp
zmq::socket_t socket{context, zmq::socket_type::dealer};
socket.set(zmq::sockopt::routing_id, "server");          // its ProcessName
socket.connect("tcp://localhost:5557");
socket.send(zmq::str_buffer("BPM"), zmq::send_flags::sndmore);
socket.send(zmq::str_buffer("alive"), zmq::send_flags::dontwait);
```

```python
socket = zmq.Context.instance().socket(zmq.DEALER)
socket.setsockopt(zmq.IDENTITY, b"server")                # its ProcessName
socket.connect("tcp://localhost:5557")
socket.send_multipart([b"BPM", b"alive"])
```

How the manager counts:
1. **Grace period:** for `GracePeriodMs` after a start, nothing is checked.
2. **Missed beats:** after that, it counts the heartbeat periods since the last heartbeat. Before the first
   heartbeat, it counts from the start, not from the end of the grace period.
3. **Unhealthy:** when the count reaches `MissedHeartbeatThreshold`, the service turns Unhealthy.
4. **What happens next:**
   - With `OnFailure`, `Always` or `UnlessStopped`, the manager stops the service and restarts it.
   - With `Never`, it stays Unhealthy and running.

So a service must send its first heartbeat within whichever is longer: `GracePeriodMs`, or
`MissedHeartbeatThreshold × HeartbeatPeriodMs`, counted from its start. Otherwise it turns Unhealthy and is
restarted. Give a slow starter a long enough `GracePeriodMs`.

### Output

| `StdioMode` | stdout and stderr go to |
|---|---|
| `Inherit` | The manager's own output. Under systemd, each service gets a journal stream of its own, so `journalctl` and the GUI show its lines under its name and pid. |
| `Pipe` | The manager's log, one line at a time, as `[name] line`. Text without a newline is logged once 1024 bytes of it have gathered. |
| `File` | `StdioFilePath`, started again at each start unless `StdioAppend` is set. Every service without its own path shares `/tmp/output.txt`. |
| `NullDevice` | Nowhere. |

### Environment and display

The variables are set in this order:
1. The service starts with the manager's environment, or with an empty one if `InheritParentEnviroment` is
   `false`. An empty environment also has no `PATH`.
2. `UseDisplay` adds `DISPLAY` and `XAUTHORITY`.
3. `EnviromentVariables` adds its variables.

### Resource figures

- **CPU and memory:** read from the service's cgroup, so they need `CreateProcessGroup` and a manager allowed to
  create cgroups (the installed unit, run as root). Without a cgroup, both read 0.
- **GPU memory and use:** sampled through the NVIDIA driver (NVML) once a second, for the main process, its
  process group and its cgroup. Without the driver, the GUI shows "GPU unavailable".

## Monitor GUI

### Starting it

```bash
process-monitor                                    # installed launcher, manager on this machine
process-monitor --host 10.0.0.5                    # a manager on another machine
python3 ProcessManagerGui/process_monitor_gui.py --host 10.0.0.5   # straight from the source folder
```

It needs Python 3 with PyQt6 and pyzmq. It runs on Linux and Windows.

| Option | Default | Meaning |
|---|---|---|
| `--host HOST` | `127.0.0.1` | The manager's machine; the default host of the four endpoints below. |
| `--sub ENDPOINT` | `tcp://HOST:6667` | The manager's health reports. |
| `--dealer ENDPOINT` | `tcp://HOST:5557` | Commands, through the broker. |
| `--network ENDPOINT` | `tcp://HOST:6669` | The broker's status. |
| `--journal ENDPOINT` | `tcp://HOST:6670` | The journal server: the logs. |

The **Connection** button in the toolbar shows a **HOST** field, so you can switch machines without restarting
the GUI. Type the manager's machine, a name or an address, and press Enter or **Reconnect**. It goes into all four
endpoints, each keeping its port, and the GUI reconnects. A pasted endpoint or `host:port` works too; only its host
is used.

The window does not show or edit the four endpoints. A port of their own, or an endpoint on another machine, is
set with the options above when the GUI starts. When the endpoints are on different machines, HOST is empty, and
**Reconnect** with HOST empty reconnects to them as they are.

### The window

- **Toolbar:**
  - the feed state: **Live** while reports arrive, **Waiting for data…** before the first one, and
    **No data for N s** when they stop;
  - the manager's machine; hovering over it lists the four endpoints;
  - the **Network** pill: red when a running service is not connected to the broker. Every running service is
    expected to be connected, the ones that send no heartbeats included, so those always count as missing;
  - the **GPU** pill;
  - **Connection**.
- **Sidebar:** the manager (**processmanager**), its **network manager** (the broker), then every service
  with a dot for its state and its CPU use. The filter box narrows the list.
- **A service's page:**
  - its state and pid, and **Start**, **Stop** and **Restart** (each asks for confirmation);
  - tiles for CPU (percent of one core), memory, GPU, VRAM, missed beats and restarts;
  - a band of its states over the last 15 minutes;
  - three tabs:
    - **Graphs**: CPU, memory, GPU and VRAM over time. **Pop out** opens them in a larger window comparing
      services.
    - **cgroup PIDs**: the processes in its cgroup on the manager's machine, with their memory and command
      line. Double-click one to open that process's log in a window of its own. The line above the table
      says why it is empty: the service is not running, it runs without a cgroup, or the journal server does
      not answer.
    - **Journal**: the service's log.
- **The manager's page:** `systemctl status` of its unit, and its own log.
- **The network manager's page:**
  - tiles for connections, messages forwarded per second, messages not delivered and messages rejected;
  - a table of the connected services, with their address, message rate and last message, and which services
    are missing;
  - the broker's events, newest first.

### Commands

**Start**, **Stop** and **Restart** behave like `--start`, `--kill` and `--restart`. The manager answers
accepted or refused, and the status bar shows the answer. Only one GUI at a time gets these answers. A second
GUI that connects takes them over, though both still see every report.

### Logs in the GUI

The GUI reads every log through the manager's journal server on port 6670. The manager runs `journalctl` (or
`systemctl status`) on its own machine and sends back what it prints, so the logs show wherever the GUI runs:
- **First and later reads:** the first read shows the newest 50 lines. Later reads, every 2 seconds, fetch only
  the lines added since, so none are skipped.
- **An older manager:** a manager from before the journal server does not answer. If it runs on the GUI's own
  machine, the GUI reads the journal there itself, as it did before.
- **cgroup PIDs:** the same server lists the processes in each service's cgroup, read from `/sys/fs/cgroup`
  and `/proc` on the manager's machine. A manager from before this gives no list; on the GUI's own machine the
  GUI then reads the cgroups itself.

### Trying the GUI without a manager

`ProcessManagerGui/mock_publisher.py` acts as a manager with six fake services, on the same ports. It publishes
reports, answers commands, and serves broker status and logs.

```bash
python3 ProcessManagerGui/mock_publisher.py
python3 ProcessManagerGui/process_monitor_gui.py
```

## Ports and network access

| Port | Direction | Used by | Content |
|---|---|---|---|
| 5557 | Clients connect to the manager | Services (heartbeats), the command line, the GUI | Message broker (ZeroMQ ROUTER) |
| 6667 | Clients subscribe | GUI, `status`, `pmt_watch` | Detailed health reports, every 500 ms and at once when a service's state changes |
| 6668 | Clients subscribe | Other systems | Short 4-byte status of the services named GGM, GGMO, YAM and TIM |
| 6669 | Clients subscribe | GUI | Broker status every second, and broker events (JSON) |
| 6670 | Clients connect to the manager | GUI | Journal server |

The manager listens on every interface. There is no authentication: anyone who can reach these ports can read
the logs and send commands. Keep them behind a firewall that admits only the machines that run the GUI.

The ports are fixed in the code, so two managers cannot run on one machine. Stop the installed service before
starting another manager by hand.

## Testing with TestServices

`ProcessManager/TestServices/` holds programs with planned behaviours and configurations that run them. Examples:
- a program that exits with a given code;
- one that crashes by a given signal;
- one that ignores SIGTERM;
- one that sends heartbeats and then stops;
- one that starts children or escapes its process group;
- one that floods its output.

`run_scenarios.py` runs the manager with each configuration and checks what it does. See
[TestServices/README.md](../ProcessManager/TestServices/README.md).

```bash
python3 ProcessManager/TestServices/scripts/run_scenarios.py --build-dir build
python3 /usr/share/processmanager/test-setup/run_scenarios.py --prefix /usr
```

The `everything` configuration runs one program of each kind, which makes a lively manager to watch in the GUI.
