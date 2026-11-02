# Architecture

How the process manager and its monitor GUI are built, and how they talk to each other. For how to use them, see
[QUICKSTART.md](QUICKSTART.md) and [USAGE.md](USAGE.md).

## Overview

```
                         manager's machine                                    any machine
 ┌──────────────────────────────────────────────────────────────────────┐
 │ iltaren-ProcessManager (one process)                                 │   ┌──────────────────┐
 │                                                                      │   │ monitor GUI      │
 │  broker ─────────────── ROUTER *:5557 ◄──── commands ────────────────┼───┤ (Python, PyQt6)  │
 │    │  status ────────── PUB    *:6669 ───── broker status ───────────┼──►│                  │
 │    │                                                                 │   │                  │
 │  supervision ── DEALER "BPM" (to the broker)                         │   │                  │
 │    │            PUB *:6667 ─────────────── health reports ───────────┼──►│                  │
 │    │            PUB *:6668 ─────────────── short status (4 bytes)    │   │                  │
 │    │                                                                 │   │                  │
 │  journal server ─ ROUTER *:6670 ◄─── journal reads, cgroup PIDs ─────┼───┤                  │
 │  GPU monitor (NVML)                                                  │   └──────────────────┘
 │  signal thread (SIGTERM, SIGINT)                                     │
 └───────┬──────────────────────────────────────────────────▲───────────┘   ┌──────────────────┐
         │ fork + exec, cgroup task_<name>                  │ heartbeats    │ command line     │
         ▼                                                  │ DEALER        │ (same machine)   │
 ┌───────────────┐ ┌───────────────┐                        │ → 5557        │ status, --start  │
 │ service A     │ │ service B     │ ───────────────────────┘               └──────────────────┘
 └───────────────┘ └───────────────┘
```

The manager is a single process that runs the services as its children. All communication goes over ZeroMQ.
- **The broker (port 5557):** relays commands from the command line and the GUI to the supervision loop, and
  heartbeats from the services.
- **Health reports (port 6667):** the supervision loop publishes the state of every service every 500 ms, and at
  once when a service's state changes.
- **Journal server (port 6670):** reads the systemd journal, and lists the processes in each service's cgroup,
  for the GUI, which may be on another machine.

## Repository layout

| Path | What it is |
|---|---|
| `ProcessManager/` | The C++20 CMake project: the manager, its modules and tests. |
| `ProcessManager/ProcessManagerApp/` | The program: `main.cpp` (threads and signals), start-up and configuration loading, and the command line (`status`, `validate`, `graph`, `--start` …). `test/keepalive.cpp` is a heartbeat example. |
| `ProcessManager/StatusTable/` | The `status` table, drawn from the health reports. |
| `ProcessManager/CommandWatch/` | Follows a command in the health reports: where each service stands. |
| `ProcessManager/StatusClient/` | The command line's ZeroMQ side: reads the reports for `status`, and sends a command and follows it. |
| `ProcessManager/ProcessManager/` | The supervision loop: services, commands, start order, health reports, GPU attribution. `ProcessManagerCommands.h` holds the command format and the endpoints. |
| `ProcessManager/Process/` | One service: its settings, starting, stopping, reaping, heartbeats, output, cgroups (`ProcessGroupBuilder`), and the health report layout (`RuntimeHealthStruct.h`). |
| `ProcessManager/NetworkManager/` | The ZeroMQ message broker. |
| `ProcessManager/ConfigSection/` | Reads the YAML configuration (the only code that uses fkYAML) and reports problems. |
| `ProcessManager/Logger/` | Logging to the console, the journal and rotating files. |
| `ProcessManager/GpuMonitor/` | GPU sampling through NVML, loaded at run time. |
| `ProcessManager/JournalServer/` | Answers journal reads for a remote GUI. |
| `ProcessManager/TestServices/` | Test programs, scenario configurations and the scenario driver. |
| `ProcessManager/packaging/` | What `cmake --install` sets up besides the programs. |
| `ProcessManager/third_party/` | libzmq, cppzmq, cxxopts, fkYAML and GoogleTest, unchanged upstream releases. |
| `ProcessManagerGui/` | The monitor GUI (Python 3, PyQt6, pyzmq). |
| `docs/` | These documents. |

There are two code styles:
- **Original modules** (`ProcessManagerApp`, `ProcessManager`, `Process`): tabs, `.h` headers, namespace
  `process_manager`.
- **Newer modules** (`NetworkManager`, `ConfigSection`, `Logger`, `GpuMonitor`, `JournalServer`,
  `TestServices`, `StatusTable`, `CommandWatch`, `StatusClient`): one concept per module, `include/`, `src/` and `tests/` folders, a snake_case namespace,
  Doxygen on every public function, and `enum class` result codes instead of exceptions.

## The manager process

### Threads

| Thread | Runs | Sockets |
|---|---|---|
| main | Start-up, then waits for the supervision thread and shuts down | — |
| signal | `sigwait` for SIGINT and SIGTERM. The first asks the supervision loop to stop; the second ends the process at once. | — |
| broker | `NetworkManager::Run`: relays messages between connected sockets | ROUTER `*:5557`, PUB `*:6669` |
| supervision | `ProcessManager::Run`: the loop below | DEALER `BPM` → 5557, PUB `*:6667`, PUB `*:6668` |
| GPU monitor | Samples NVML every second; a call stuck for 2 s is left behind | — |
| journal server | Answers journal reads, running `journalctl` or `systemctl` for each, and lists the services' cgroups | ROUTER `*:6670` |

libzmq adds its own I/O threads. SIGINT and SIGTERM are blocked in every thread, so only the signal thread
receives them; services start with no signal blocked.

### Start-up

1. **Command line:** read with cxxopts. `validate`, `graph`, `generate`, `status` and the command options do their
   work and exit here.
2. **Logger:** opened, with the log file directory.
3. **Configuration:** loaded with `ConfigSection`. The YAML document is kept, because `--start all` rebuilds the
   services from it.
4. **Services:** `ProcessManager::GenerateConfig` turns the document into a list of `Process` objects. Unknown
   keys and bad values are logged.
5. **Sockets and cgroups:** the `ProcessManager` constructor binds 6667 and 6668 and connects its DEALER to the
   broker's address (the broker itself is bound in step 7; ZeroMQ connects once it is). Then `ProcessGroupBuilder` creates `task_pm`, moves the manager into it, and enables the `cpu` and
   `memory` controllers for the service cgroups.
6. **Order and checks:** the services are sorted by `DependsOn` (Kahn's algorithm), and each one's paths are
   checked. A cycle ends the start-up.
7. **Broker:** bound on 5557, with its status publisher on 6669. A failed bind ends the start-up before any
   service starts.
8. **Threads:** the signal, broker and supervision threads start. The supervision thread starts the GPU monitor
   and the journal server.

### The supervision loop

One pass of `ProcessManager::Run`:

```
start the next due Auto service (start order, grace periods, restart delays)
take the newest GPU sample
for each service:  check heartbeats → advance a stop in progress → reap an exit (waitpid WNOHANG)
                   → read piped output → build its health report (with its GPU use)
for each service being retired (--disable, --start all): advance its stop, reap it
if 500 ms have passed since the last report, or a service's state, pid or restart count changed:
                   publish the detailed reports on 6667 and the short status on 6668
wait for messages on the DEALER, until the next report is due (at most 50 ms while a stop is in progress)
handle up to 64 messages: commands from PMC or GUI, heartbeats from services
```

The start step goes through the services in start order. A service in its grace period, or being stopped, ends
the step for every service after it, so their starts, automatic restarts and stops for missed heartbeats wait.

Every message that arrives starts a pass, so heartbeats keep the passes frequent, but the reports still come
every 500 ms. A report on every pass would make each subscriber's traffic grow with the square of the number of
services: N services beating every P seconds start N/P passes a second, each with a report of 144 × N bytes. A
start, stop, restart, or a service added or removed goes out at once, so a client sees it even when it is over
within 500 ms.

Each service is reaped with
`waitpid(pid)`, never `waitpid(-1)`, so other children of the manager, such as the journal server's
`journalctl`, are left to their owners.

### Shutdown

1. **Stopping the services:** a signal (or a broker failure) asks the loop to stop, and the loop ends.
2. **Waiting for them:** the supervision thread then sends every service SIGTERM at once and waits for all of
   them, escalating as usual. Nothing is published on 6667 and 6668, and no command is taken, during this wait.
3. **Ending the process:** the supervision thread returns. Main closes the manager, logs, and ends the process
   with `_Exit(0)`, without unwinding the broker and signal threads, which have no way to stop. The exit code is
   0 after a broker failure too.

## Messaging

### The broker

`NetworkManager` is a ZeroMQ ROUTER:
- **Routing:** a connected DEALER sends `[destination][payload…]`, and the broker delivers `[source][payload…]` to
  the destination.
- **Identities:** each sender is known by its ZeroMQ identity:

| Identity | Who |
|---|---|
| `BPM` | The supervision loop |
| `PMC` | The command line (`--start`, `--kill`, …) |
| `GUI` | The monitor GUI |
| *ProcessName* | A service sending heartbeats |

- **Delivery:** a send never blocks. A message for an identity that is not connected is dropped. The broker logs
  a warning, repeated at most once per 10 s for the same sender and destination, with the count held back.
- **Takeover:** a second connection with the same identity takes it over (`ROUTER_HANDOVER`). The broker reads
  nothing more from the first one, which stays open, so what is sent on it vanishes.
- **Status:** the broker publishes on 6669, as JSON with `"version": 1`.
  - Topic `status` comes every second: who is connected, from where, since when and how many messages each sent.
  - Topic `event` comes for every join, leave and takeover, and for every warning that is logged, so undelivered
    messages are throttled there too.

### Commands

`PMC` or `GUI` sends `[BPM][command]`, where the command is a 65-byte `ProcessManagerCommandStruct`:

| Offset | Field | Type |
|---|---|---|
| 0 | `command` | `uint8` |
| 1 | `ServiceName` | `char[32]`, NUL-terminated; `all` means every service |
| 33 | `Args` | `char[32]` |

| Code | Command |
|---|---|
| 77 | Start with other arguments (no command-line option uses it) |
| 78 | Start |
| 79 | Stop |
| 80 | Disable |
| 81 | Restart |
| 82 | Version information (answered with a `VersionReportStruct`) |

The manager answers the `GUI` identity only, with one frame: `1` for accepted, `0` for refused. The command line
gets no answer.

Every GUI has the identity `GUI`. Because of the takeover, a GUI whose connection another GUI took over, or whose
idle connection a firewall dropped without a word, would send into a connection that goes nowhere. So the GUI
opens a new connection for each command, unless the previous command still waits for its answer. A command with
no answer after 10 s is reported as such.

### Heartbeats

A service with the identity of its `ProcessName` sends `[BPM][anything]`. The supervision loop takes any message
of two or more frames from a known `ProcessName` as a heartbeat. The content is ignored.

### Health reports (port 6667)

Each publish is one frame: the concatenated reports of every service, 144 bytes each, in the machine's byte order,
with no count in front.

Managers older than GPU reporting sent 128-byte reports. A frame of 9, 18, … such reports also divides into 144-byte
ones, so the GUI checks more than the size: GPU memory may be non-zero only in a report whose GPU state is
Sampled. The GUI shows a frame that fails these checks as a parse error in its status bar.

| Offset | Field | Type | Notes |
|---|---|---|---|
| 0 | `processName` | `char[64]` | NUL-terminated |
| 64 | `pid` | `int32` | -1 when not running |
| 72 | `memoryUsageInBytes` | `uint64` | `memory.current` of its cgroup |
| 80 | `cpuUsageInUsec` | `uint64` | `usage_usec` of its cgroup's `cpu.stat`; the readers take differences |
| 88 | `state` | `uint8` | 0 Unknown, 1 Starting, 2 Running, 3 Stopped, 4 Unhealthy |
| 96 | `startTime` | `int64` | ns since the epoch |
| 104 | `lastSeen` | `int64` | ns since the epoch: the last heartbeat, or the last start if later |
| 112 | `missedBeats` | `int32` | |
| 116 | `restartCount` | `int32` | Every restart, the ones asked for included |
| 120 | `snapshotTime` | `int64` | ns since the epoch |
| 128 | `gpu.memoryUsageInBytes` | `uint64` | 0 unless the GPU state is Sampled |
| 136 | `gpu.utilizationPercent` | `int32` | Summed over its processes and GPUs; -1 unknown |
| 140 | `gpu.state` | `uint8` | 0 not sampled, 1 sampled, 2 no driver, 3 no device, 4 failed, 5 stalled |
| 141 | reserved | `uint8[3]` | Zero |

The C++ layout is `DetailedHealthReportStruct` in `Process/source/include/RuntimeHealthStruct.h`. The Python
layout is `DetailedHealthReport` in `ProcessManagerGui/health_structs.py`. A `static_assert` and a Python
`assert` keep both at 144 bytes.

The short status on 6668 is 4 bytes, one state each, kept for an older consumer:
- byte 0: the service named GGM;
- byte 1: GGMO, but Running whenever GGM is running;
- byte 2: YAM;
- byte 3: TIM.

### Journal reads and cgroup PIDs (port 6670)

A REQ or DEALER client sends one of the requests below, and gets `["ok", output]` or `["error", text]` back:

| Request | Runs on the manager's machine |
|---|---|
| `["version"]` | Nothing; answers `2` (version 1 had no `members`) |
| `["journal", "manager", "", lines, cursor]` | `journalctl -u <the manager's unit>`, or `_PID=<the manager's pid>` outside a unit |
| `["journal", "service", NAME, lines, cursor]` | `journalctl _SYSTEMD_CGROUP=<the manager's cgroup>/task_NAME` |
| `["journal", "pid", PID, lines, cursor]` | `journalctl _PID=PID` |
| `["unit-status"]` | `systemctl status <the manager's unit> --no-pager -l -n 0` |
| `["members"]` | Nothing; reads `cgroup.procs` of every `task_<name>` next to `task_pm`, then `/proc/<pid>` |

The `members` answer has one line per cgroup and one per process in it, with tab-separated fields:
```
cgroup	GGM
pid	GGM	1234	52428800	python3	python3 /home/user/main.py
```
- **Process fields:** the service name, the pid, the resident memory in bytes (`VmRSS`), `comm`, and the command
  line with its arguments separated by spaces, cut after 4096 bytes. Control characters in `comm` and the command
  line become spaces.
- **Which cgroups:** `task_pm`, the manager's own, is left out. A service without a cgroup has no line; the
  manager removes a service's cgroup when the service stops.
- **A process that has gone** between the two reads keeps its pid, with empty fields and 0 memory.
- **Errors:** the answer is an error only when the manager's cgroup folder cannot be listed. A manager without
  cgroups (see [cgroups](#cgroups)) answers with no lines.

How a journal read runs:
- **Arguments:** every journal read adds `-n <lines> --no-pager --all --quiet -o json`, and
  `--after-cursor=<cursor>` when a cursor is given.
- **Without a cursor:** journalctl prints the newest `lines` entries.
- **With a cursor:** it prints the first `lines` entries after the cursor. The GUI sends the `__CURSOR` of the
  last entry it has, so it gets every new entry exactly once.

How the server runs the program:
- **Arguments:** built from a fixed list. Every value is checked first: the service name, the pid, the cursor, and
  a line count of digits only, which is then clamped to 1–1000.
- **Running:** with `posix_spawnp` and no shell. The program gets at most 5 s and 4 MB of output, and is killed
  when the server stops.
- **The manager's unit and cgroup:** found from the cgroup that `ProcessGroupBuilder` found at start-up.

## Service supervision

### States

```
                 start                 first heartbeat
   Unknown ──────────────► Starting ───────────────────► Running ◄──┐
      │        (no CheckHealth: straight to Running)       │  ▲      │ heartbeats again
      │                                                    │  │      │
      │                                   missed threshold │  └──────┤
      │                                                    ▼         │
      │                                                Unhealthy ────┘
      │                                                    │ stop (RestartMode other than Never)
      ▼                                                    ▼
   Stopped ◄───────────────── exit or stop ─────────────── any running state
      │
      └── restart policy: wait RestartDelayMs × BackoffMultiplierⁿ (≤ 5 min), then start again
```

### Stopping

```
Terminating ── SIGTERM to the process group; deadline = now + ForceKillTimeoutMs
     │ main process reaped and group empty ──────────────────────────────► done
     │ deadline passed
     ▼
Killing ────── SIGKILL to the group, cgroup.kill; deadline = now + ForceKillTimeoutMs
     │ main process reaped and group empty ──────────────────────────────► done
     │ deadline passed
     ▼
"Force shutdown failed" logged; the stop is abandoned
```

When a stop is done, the manager also kills whatever is left in the service's cgroup and removes the cgroup.

Whether an exit is restarted depends on three things:
- **Why it stopped:** an exit code, a signal, or a stop for missed heartbeats.
- **`RestartMode`.**
- **The work mode:** a stop command sets the service to Manual, which keeps it stopped.

`MaxRestarts` limits restarts in a row, and a run of `RestartResetMs` or longer starts that count again.

### cgroups

The installed systemd unit has `Delegate=yes`, so the manager may manage the cgroups below its own:

```
/sys/fs/cgroup/system.slice/processmanager.service/          the unit's cgroup ("+cpu +memory" for its children)
├── task_pm/            the manager itself
├── task_server/        one per service with CreateProcessGroup: true
└── task_viewer/
```

- **Manager first:** the manager moves itself into `task_pm` before it enables the controllers, as cgroup v2
  requires.
- **Placing a service:** the child writes its own pid into `task_<name>/cgroup.procs` before `exec`.
- **Fallback:** if the cgroup cannot be created, for example without permission or without cgroup v2, the
  service runs without one. Its CPU and memory then read 0, and a stop reaches only its process group.

### Output and the journal

| `StdioMode` | Implementation |
|---|---|
| `Inherit` | The child keeps the manager's stdout and stderr. Under journald (`JOURNAL_STREAM` set), the child opens a journal stream of its own, named after its `ProcessName`. journald then records its lines under its own pid and cgroup, and the journal server finds them by cgroup. |
| `Pipe` | Both go to one pipe. The manager reads its end without blocking, up to 16 × 4 KB per pass, and logs each line as `[name] text`. A child that writes faster blocks when the pipe is full. |
| `File` | `open` and `dup2` onto `StdioFilePath`, truncated or appended. |
| `NullDevice` | `/dev/null`. |

### Resource figures

- **CPU and memory:** read from the service's cgroup files on every pass.
- **GPU:** `GpuMonitor` loads `libnvidia-ml.so.1` with `dlopen`, so the build needs no NVIDIA driver. It samples
  the GPU memory and use of every process on every GPU, once a second, on its own thread.
- **GPU attribution:** on the supervision thread, `GpuUsageReporter` adds up the use of each service's processes:
  its main pid, its process group, and anything in its cgroup.
- **Stalled samples:** a sample older than 5 s is reported as stalled, because a driver call has stopped
  returning.

## Configuration

The configuration is read in four steps:
1. **YAML:** `ConfigSection` reads the file with fkYAML into a `ConfigValue` tree. Syntax errors carry their line
   and column.
2. **Keys:** each part of the program reads its own keys through a `ConfigSection`. `ProcessConfigParams`,
   `RestartPolicyStruct`, `HealthPolicyStruct` and `StdioPolicyStruct` give the defaults and the limits.
3. **Problems:** the `ConfigSection` records what it found: unknown keys, values of the wrong kind, and clamped
   numbers.
4. **Reporting:** `validate` prints the problems, and the manager logs them at start-up.

The manager reads the file once, at start-up. `--start all` rebuilds the services from the document it kept then.

## Logging

`Logger` is printf-style and thread-safe, with two outputs:
- **stderr:** with journald priority prefixes (`<3>` …) when stderr is the journal, and with colours on a
  terminal.
- **A file:** `PMLog_<date>_<time>.log`, continued in a new file at 100 MB.

A forked child never logs, because it may hold no lock between `fork` and `exec`.

## Monitor GUI

| Module | Role |
|---|---|
| `process_monitor_gui.py` | The main window, the worker threads, and the command-line entry point. |
| `process_views.py` | Sidebar, service page (tiles, state band, tabs), manager page. |
| `network_view.py`, `network_status.py` | The broker page, and parsing of the broker's JSON. |
| `usage_graphs.py` | Usage history (15 minutes) and the graphs. |
| `journal_view.py` | Log text view: one colour per pid, new lines appended in place. |
| `remote_journal.py` | Client of the journal server: follows a journal with cursors and lists the cgroups' processes. For an older manager on the GUI's machine it falls back to local `journalctl` and `/sys/fs/cgroup`. |
| `log_recorder.py`, `record_view.py` | Recording services' logs into files on the GUI's machine, and the **Record logs** dialog. |
| `systemd_logs.py` | Local `journalctl`, `systemctl` and cgroup reading, and the journal JSON parser. |
| `health_structs.py` | The binary layouts of reports and commands. |
| `ui_scale.py` | Font and window sizes that follow the screen. |
| `window_frame.py` | The main window without the system title bar: the toolbar's row is the title bar, with the window buttons, and the window's border resizes it. |
| `mock_publisher.py` | A stand-in manager for working on the GUI. |

Each data source has a worker on its own `QThread`. A worker sends Qt signals to the window, which updates the
views:

| Worker | Reads | Every |
|---|---|---|
| `ZmqWorker` | Reports on 6667, keeping only the newest. It also sends commands as `GUI` through 5557, each on a new connection, and reads the answers. | As they come |
| `NetworkWorker` | Broker status on 6669 | As it comes |
| `SystemdLogWorker` | The manager's unit status and journal, through 6670 | 2 s |
| `DetailJournalWorker` | The selected service's journal, through 6670 | 2 s, and at once on a new selection |
| `CgroupLogWorker` | One process's journal, for a log window | 2 s |
| `CgroupMembersWorker` | The processes in each service's cgroup, through 6670 | 2 s |
| `LogRecordWorker` | The recorded services' journals, through 6670, written to their files | 2 s, and again at once for one that is behind |

Each report holds every service, so an older one has nothing a newer one lacks. `ZmqWorker` reads every report
waiting on its socket and keeps the last, and it signals the window only after the window has taken the previous
one. A window that is busy then skips reports instead of falling further behind.

The window computes CPU percent from the difference between two reports. It marks the feed stale when reports
stop, and keeps a 15-minute history for the state band and the graphs.

## Build and install

- **The root `CMakeLists.txt`:**
  - sets C++20 (required, no extensions);
  - adds the vendored libraries: all but fkYAML as `EXCLUDE_FROM_ALL`, and all but GoogleTest with their include
    directories marked as system ones;
  - defines the install paths (`PM_*_DIR`);
  - adds each module.
- **Each module's `CMakeLists.txt`:** owns its targets.
- **Linking:** libzmq is linked statically, so the installed programs need no shared libraries beyond the C and
  C++ runtimes.
- **`packaging/`:** installs the configuration (keeping an existing one), the log directory, the systemd unit and
  the GUI. Last, it enables the unit when that is possible.

## Tests

| Suite | Tests | Covers |
|---|---|---|
| `config_section_tests` | 23 | YAML reading, defaults, problems |
| `logger_tests` | 13 | Levels, files, rotation, journald prefixes |
| `network_manager_tests` | 35 | Routing, identities, status and events |
| `gpu_monitor_tests` | 27 | NVML stand-in: sampling, process lists, stuck calls |
| `journal_server_tests` | 42 | Argument building, the request protocol, stand-in `journalctl` scripts, cgroup listing |
| `test_service_tests` | 10 | The TestServices helpers |
| `status_table_tests` | 19 | The `status` table: columns, CPU figures, header, colours |
| `command_watch_tests` | 17 | Following start, stop, restart and disable, for one service and for all |
| `status_client_tests` | 13 | Report decoding, `status --once` and commands against a stand-in manager |

`ctest` runs all of them. Two more checks run outside ctest:
- `TestServices/scripts/run_scenarios.py` runs the real manager against nine scenario configurations, covering
  start order, restarts, stopping, health checks, process trees, output, environment and resource figures. It
  needs the manager's fixed ports, so ctest does not run it.
- The GUI is checked against `mock_publisher.py` or a running manager.

## Known limits

- **No authentication:** the ports listen on every interface. Use a firewall.
- **Fixed ports:** they are compile-time constants, so one manager per machine.
- **GUIs share one identity:** when two GUIs send commands at the same moment, an answer can reach the other
  GUI.
- **Configuration at start-up only:** the file is read once; restart the manager after changing it.
- **Dependencies only order starts:** stopping or losing a service does not stop the services that depend on it,
  and `--start` starts only direct dependencies.
- **Heartbeats count from the start:** a service that sends no heartbeat within the longer of `GracePeriodMs` and
  `MissedHeartbeatThreshold × HeartbeatPeriodMs` after its start is restarted before it beats.
- **Grace periods hold up the start order:** while one service is in its grace period, the services after it
  wait.
