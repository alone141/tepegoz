# Architecture

How Tepegöz, the process manager, and its monitor GUI, Göz, are built, and how they talk to each other. For how to use them, see
[QUICKSTART.md](QUICKSTART.md) and [USAGE.md](USAGE.md).

## Overview

```
                         manager's machine                                    any machine
 ┌──────────────────────────────────────────────────────────────────────┐
 │ tepegoz (one process)                                                │   ┌──────────────────┐
 │                                                                      │   │ monitor GUI      │
 │  broker ─────────────── ROUTER *:5557 ◄──── commands ────────────────┼───┤ (Python, PyQt6)  │
 │    │  status ────────── PUB    *:6669 ───── broker status ───────────┼──►│                  │
 │    │                                                                 │   │                  │
 │  supervision ── DEALER "BPM" (to the broker)                         │   │                  │
 │    │            PUB *:6667 ─────────────── health reports ───────────┼──►│                  │
 │    │                                                                 │   │                  │
 │  journal server ─ ROUTER *:6670 ◄─── journal reads, cgroup PIDs ─────┼───┤                  │
 │  registry ─ ROUTER 127.0.0.1:6671 ◄─── register, change, remove ─────┼───┤                  │
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
- **Service registry (port 6671):** registers, changes and removes services on request, by writing their files in
  `services.d` and reloading. It listens on this machine only unless `RemoteRegistration: true`.

## Repository layout

| Path | What it is |
|---|---|
| `ProcessManager/` | The C++20 CMake project: the manager, its modules and tests. |
| `ProcessManager/ProcessManagerApp/` | The program: `main.cpp` (threads and signals), start-up and configuration loading, and the command line (`status`, `validate`, `graph`, `reload`, `add`, `list`, `--start` …). `tests/keepalive.cpp` is a heartbeat example. |
| `ProcessManager/StatusTable/` | The `status` table, drawn from the health reports, with the restart-to-apply mark. |
| `ProcessManager/CommandWatch/` | Follows a command in the health reports: where each service stands. |
| `ProcessManager/StatusClient/` | The command line's ZeroMQ side: reads the reports for `status`, sends a command and follows it, and sends a reload and tells its answer. |
| `ProcessManager/RegistryClient/` | The command line's `add`, `set`, `replace`, `remove`, `show` and `list`: builds a registry request from the words typed, sends it, and words the answer and the `list` table. |
| `ProcessManager/ProcessManager/` | The supervision loop: services, commands, start order, health reports, GPU attribution. `TepegozCommands.hpp` holds the command format and the endpoints. |
| `ProcessManager/Process/` | One service: its settings, starting, stopping, reaping, heartbeats, output, cgroups (`ProcessGroupBuilder`), and the health report layout (`RuntimeHealth.hpp`). |
| `ProcessManager/NetworkManager/` | The ZeroMQ message broker. |
| `ProcessManager/ConfigSection/` | Reads the YAML configuration (the only code that uses fkYAML) and reports problems. |
| `ProcessManager/ConfigSource/` | Reads the main configuration file and the `services.d` folder next to it into one document, and keeps where each service was defined and whether the folder could be listed. |
| `ProcessManager/ReloadPlan/` | Compares the services the manager has with a configuration read again: added, removed, changed, kept, unchanged, or why the reload is refused. |
| `ProcessManager/ServiceRegistry/` | The service registry's parts that need no running manager: requests, names, editing a definition, writing it as YAML and JSON, and the files of `services.d` with snapshots to undo a change. |
| `ProcessManager/Logger/` | Logging to the console, the journal and rotating files. |
| `ProcessManager/GpuMonitor/` | GPU sampling through NVML, loaded at run time. |
| `ProcessManager/JournalServer/` | Answers journal reads for a remote GUI. |
| `ProcessManager/TestServices/` | Test programs, scenario configurations and the scenario driver. |
| `ProcessManager/packaging/` | What `cmake --install` sets up besides the programs. |
| `ProcessManager/third_party/` | libzmq, cppzmq, cxxopts, fkYAML and GoogleTest, unchanged upstream releases. |
| `ProcessManagerGui/` | Göz, the monitor GUI (Python 3, PyQt6, pyzmq). |
| `docs/` | These documents. |

Every module has the same layout and naming: `.hpp` headers in `include/`, sources in `src/`, tests in
`tests/`, 4-space indentation, PascalCase types and functions, camelCase members. Two things still set
the original modules (`TepegozApp`, `Tepegoz`, `Process`) apart from the newer ones:
- **Namespace:** they share `tepegoz`, where each newer module has a snake_case namespace of
  its own.
- **Public API:** their functions have no Doxygen blocks yet, and they report a failure as a `bool` or
  an `int` code, where the newer modules return `enum class` codes.

## The manager process

### Threads

| Thread | Runs | Sockets |
|---|---|---|
| main | Start-up, then waits for the supervision thread and shuts down | — |
| signal | `sigwait` for SIGINT and SIGTERM. The first asks the supervision loop to stop; the second ends the process at once. | — |
| broker | `NetworkManager::Run`: relays messages between connected sockets | ROUTER `*:5557`, PUB `*:6669` |
| supervision | `Tepegoz::Run`: the loop below | DEALER `BPM` → 5557, PUB `*:6667`, ROUTER `127.0.0.1:6671` (`*:6671` with `RemoteRegistration: true`) |
| GPU monitor | Samples NVML every second; a call stuck for 2 s is left behind | — |
| journal server | Answers journal reads, running `journalctl` or `systemctl` for each, and lists the services' cgroups | ROUTER `*:6670` |

libzmq adds its own I/O threads. SIGINT and SIGTERM are blocked in every thread, so only the signal thread
receives them; services start with no signal blocked.

### Start-up

1. **Command line:** read with cxxopts. `validate`, `graph`, `generate`, `status` and the command options do their
   work and exit here.
2. **Logger:** opened, with the log file directory.
3. **Configuration:** loaded with `ConfigSource`: the main file, then each file of `services.d`, whose service is
   appended to the main file's `Services`. The merged document is kept, because `--start all` rebuilds the
   services from it. So are the loader and where each service was defined, for a reload.
4. **Services:** `Tepegoz::GenerateConfig` turns the document into a list of `Process` objects. Unknown
   keys and bad values are logged.
5. **Sockets and cgroups:** the `Tepegoz` constructor binds 6667 and connects its DEALER to the
   broker's address (the broker itself is bound in step 7; ZeroMQ connects once it is). Then `ProcessGroupBuilder` creates `task_pm`, moves the manager into it, and enables the `cpu` and
   `memory` controllers for the service cgroups.
6. **Order and checks:** the services are sorted by `DependsOn` (Kahn's algorithm), and each one's paths are
   checked. A cycle ends the start-up.
7. **Broker:** bound on 5557, with its status publisher on 6669. A failed bind ends the start-up before any
   service starts.
8. **Threads:** the signal, broker and supervision threads start. The supervision thread starts the GPU monitor
   and the journal server.

### The supervision loop

One pass of `Tepegoz::Run`:

```
start the next due Auto service (start order, grace periods, restart delays)
take the newest GPU sample
for each service:  check heartbeats → advance a stop in progress → reap an exit (waitpid WNOHANG)
                   → read piped output → build its health report (with its GPU use)
for each service being retired (--disable, --start all): advance its stop, reap it
if 500 ms have passed since the last report, or a service's state, pid, restart count or mark changed:
                   publish the detailed reports on 6667
wait for messages on the DEALER and the registry, until the next report is due (at most 50 ms while a stop is
                   in progress)
handle up to 64 messages: commands from PMC or GUI (a reload among them), heartbeats from services
answer up to 16 service registry requests
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
   them, escalating as usual. Nothing is published on 6667, and no command is taken, during this wait.
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

`PMC` or `GUI` sends `[BPM][command]`, where the command is a 65-byte `CommandMessage`:

| Offset | Field | Type |
|---|---|---|
| 0 | `command` | `uint8` |
| 1 | `serviceName` | `char[32]`, NUL-terminated; `all` means every service |
| 33 | `args` | `char[32]` |

| Code | Command |
|---|---|
| 77 | Start with other arguments (no command-line option uses it) |
| 78 | Start |
| 79 | Stop |
| 80 | Disable |
| 81 | Restart |
| 82 | Version information (answered with a `VersionReport`) |
| 83 | Reload the configuration (answered with a `ReloadReport`); `serviceName` is not used |

The manager answers the `GUI` identity only, with one frame: `1` for accepted, `0` for refused. The command line
gets no answer, except to a reload.

A reload is answered to whoever sent it, `PMC` or `GUI`, with one frame: a packed 16-byte `ReloadReport`. When
`args` is `no-reply` (`reload --no-wait`), no answer is sent, so the broker has nothing to drop.

| Offset | Field | Type | Notes |
|---|---|---|---|
| 0 | `command` | `uint8` | 83 |
| 1 | `result` | `uint8` | 0 applied; refused: 1 not read from a file, 2 main file not opened, 3 not valid YAML, 4 no `Services` list, 5 `services.d` not listable, 6 dependency cycle, 7 two services of one name |
| 2 | `added` | `uint16` | Each count in the machine's byte order, 0 unless applied |
| 4 | `removed` | `uint16` | |
| 6 | `changed` | `uint16` | Their new definition waits for their next start |
| 8 | `kept` | `uint16` | Their new definition could not be read |
| 10 | `skipped` | `uint16` | New, but their definition could not be read |
| 12 | `unchanged` | `uint16` | |
| 14 | reserved | `uint8[2]` | Zero |

`reload` waits up to 10 s for the answer. When it is applied, it takes the first health report made after the
answer and names the services added, removed and marked, by comparing it with the last report before the
reload. The kept and skipped services are only counted; the manager's log names them.

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
| 89 | `restartToApply` | `uint8` | 1 while a reloaded definition waits for the service's next start, else 0 |
| 90 | padding | `uint8[6]` | Zero |
| 96 | `startTime` | `int64` | ns since the epoch |
| 104 | `lastSeen` | `int64` | ns since the epoch: the last heartbeat, or the last start if later |
| 112 | `missedBeats` | `int32` | |
| 116 | `restartCount` | `int32` | Every restart, the ones asked for included |
| 120 | `snapshotTime` | `int64` | ns since the epoch |
| 128 | `gpu.memoryUsageInBytes` | `uint64` | 0 unless the GPU state is Sampled |
| 136 | `gpu.utilizationPercent` | `int32` | Summed over its processes and GPUs; -1 unknown |
| 140 | `gpu.state` | `uint8` | 0 not sampled, 1 sampled, 2 no driver, 3 no device, 4 failed, 5 stalled |
| 141 | reserved | `uint8[3]` | Zero |

The C++ layout is `DetailedHealthReport` in `Process/include/RuntimeHealth.hpp`. The Python
layout is `DetailedHealthReport` in `ProcessManagerGui/health_structs.py`. A `static_assert` and a Python
`assert` keep both at 144 bytes, and further `static_assert`s keep every field at its offset. `restartToApply`
took a byte that was padding after `state`, so an older reader still parses the report and ignores the mark.

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
cgroup	sensor_fusion
pid	sensor_fusion	1234	52428800	python3	python3 /home/user/main.py
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

### Service registry (port 6671)

The supervision loop answers requests to register, change and remove services, on a ROUTER bound to
`127.0.0.1:6671`, or to `*:6671` with `RemoteRegistration: true`. A bind that fails is logged, and the manager
supervises without the registry. A REQ or DEALER client sends `[operation, parameters]` and gets `[status,
object]` back:
- **operation:** `version`, `list`, `show`, `add`, `replace`, `update` or `remove`.
- **parameters:** one YAML or JSON mapping. `version` and `list` take none, and the frame can be left out. A
  request has at most two frames and 64 KiB, and an unknown parameter is an error.
- **status:** `ok`; `refused`, when nothing was written, with `message` and `problems` saying why; or `error`,
  when the request was not understood, with `message` saying why.
- **object:** one JSON object. Every one has `version`, the protocol version (1), and `message`, a sentence for a
  person.

| Operation | Parameters | `ok` object, besides `version`, `operation` and `message` |
|---|---|---|
| `version` | none | `folder`: the `services.d` folder |
| `list` | none | `folder`, and `services`: one object per service, below |
| `show` | `name` | `name`, `origin`, `file`, `enabled`, `definition` (the service's keys as a JSON object), `yaml`, and the run state. For a file of `services.d` that is skipped: `definition` is `null`, with `problem` and `fileText` |
| `add` | `name`, `definition`, `start` (default true), `dryRun` | `name`, `outcome`, `file`, `yaml`, `reload` |
| `replace` | `name`, `definition`, `restart`, `dryRun` | the same |
| `update` | `name`, `set` (keys and their new values), `unset` (a key, or a list of keys), `restart`, `dryRun` | the same |
| `remove` | `name`, `force`, `dryRun` | `name`, `outcome`, `file`, `reload` |

- **A service in `list`:** `name`, `origin` (`main` or `registered`), `file`, `enabled`, `loaded` (the manager
  has it), `state`, `pid` (-1 when it does not run), `workMode`, `restartToApply`, and `problem` for a file of
  `services.d` that is skipped.
- **`outcome`:** `starting`; `held` (added with `start: false`); `manual` (its WorkMode is Manual); `disabled`
  (`Enabled: false`); `waiting` (a changed service waits for its next start); `restarting`; `applied` (a stopped
  service took its new definition); `unchanged`; `removed`; or `valid` for a dry run.
- **`reload`:** the counts of the reload that applied the change: `added`, `removed`, `changed`, `kept`,
  `skipped` and `unchanged`.
- **A refusal:** `name` when the request had one, and `problems`: the problems the change would add, or the
  services that need the one being removed.

The command line is one client (`RegistryClient`): `add`, `replace`, `remove`, `show` and `list` send the
operation of their name, and `set` sends `update`. It sends through a DEALER, waits up to 10 s for the answer, and
gives up after 2 s when the request cannot be sent, as the other commands do. It reads the answer with
`ConfigSection`, since JSON is YAML. A client in Python needs only pyzmq and json:

```python
socket = zmq.Context().socket(zmq.DEALER)
socket.connect("tcp://localhost:6671")
definition = {"ExecutablePath": "/opt/cam/bin/server", "Args": ["--port", "8080"]}
socket.send_multipart([b"add", json.dumps({"name": "camera", "definition": definition}).encode()])
status, reply = socket.recv_multipart()   # b"ok", and the JSON object
```

How a change is made, in `Tepegoz::HandleRegistry` (`TepegozRegistry.cpp`):
1. **Name:** `service_registry::CheckName` and the reserved names. `replace`, `update` and `remove` need a file
   in `services.d` and refuse a service of the main file; `add` refuses a name that is anywhere already.
2. **Definition:** `add` and `replace` take the one sent; `update` reads the file and applies `unset`, then `set`.
   `ProcessName` is set from the name, and a definition that states another one is refused.
3. **Checking:** `ConfigSource::Load` with a `DropInOverride` reads the configuration as the change would leave
   it, without touching the folder. The problems of both configurations are found as `validate` finds them, and
   any problem only the new one has refuses the request. A `remove` with `force` leaves out the missing
   dependencies of the services it was told to leave.
4. **Writing:** `ServiceFiles` keeps a `Snapshot` of the file and the folder, then writes the YAML `EmitYaml` made
   to a dot file, which is renamed over the file, or deletes the file.
5. **Reloading:** `Tepegoz::Reload` applies the change. If the reload is refused, the snapshot is put back.
6. **Answering:** the outcome is what the reload did to the service. `start: false` makes the new service Manual
   in the manager only, and `restart: true` restarts a running service, or applies the waiting definition of a
   stopped one.

`EmitYaml` writes a text without quotes only when the reader reads it back as that same text, and quotes it
otherwise, so `"8080"`, `"true"` and `":1"` stay texts. A float keeps a decimal point (`2.0`), so it reads back
as a float. The tests read every written file back and compare the keys and values.

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

A main process that exits by itself starts the same two steps for what it left in its process group, and a kill
of its cgroup. A start or restart waits until they are done: `StartProcess` turns into a restart request, and
`NeedsRestart` says no while the stop runs.

Whether an exit is restarted depends on three things:
- **Why it stopped:** an exit code, a signal, or a stop for missed heartbeats.
- **`RestartMode`.**
- **The work mode:** a stop command sets the service to Manual, which keeps it stopped.

`MaxRestarts` limits restarts in a row, and a run of `RestartResetMs` or longer starts that count again.

### cgroups

The installed systemd unit has `Delegate=yes`, so the manager may manage the cgroups below its own:

```
/sys/fs/cgroup/system.slice/tepegoz.service/          the unit's cgroup ("+cpu +memory" for its children)
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
   and column. `ConfigSource` reads the main file this way, then each `*.yaml` file of the `services.d` folder next
   to it, in file name order, and appends each one's service to the main file's `Services`. It skips, and
   reports, a file that is not valid YAML or not a mapping, whose `ProcessName` is not its file name, or whose
   name the main file already has. For each service it records where it was defined: the main file, or which file
   of `services.d`. For each file of `services.d` it records whether its service was added or why not.
2. **Keys:** each part of the program reads its own keys through a `ConfigSection`. `ProcessConfigParams`,
   `RestartPolicy`, `HealthPolicy` and `StdioPolicy` give the defaults and the limits.
3. **Problems:** the `ConfigSection` records what it found: unknown keys, values of the wrong kind, clamped
   numbers, and old spellings of renamed keys (`RenamedKey`).
4. **Reporting:** `validate` prints the problems, and the manager logs them at start-up.

The manager reads the file and its `services.d` at start-up and on every reload. `--start all` rebuilds the
services from the document it kept the last time.

### Reloading

A reload (command 83) runs in the supervision loop, between two passes, like any command, so it needs no lock:
1. **Reading:** `ConfigSource` reads the file and `services.d` again. A file that cannot be opened or is not
   valid YAML refuses the reload.
2. **Building:** `GenerateConfig` and `SortByDependencies` build the new services as start-up would. A dependency
   cycle refuses the reload.
3. **Planning:** `reload_plan::PlanReload` compares, by `ProcessName`, the services the manager has with the
   ones built, using the drop-in results to tell a deleted file from one that cannot be read. It refuses a
   folder that could not be listed, a main file without a `Services` list, and two services of one name. A
   service is changed when its own entry differs from the one in the document kept last; key order, comments and
   layout do not count.
4. **Applying:** the removed services are retired as `--disable` retires them. A changed one keeps running and
   gets the new definition through `Process::DeferDefinition`. The new list is the kept services, then the new
   start order with each existing `Process` moved into it and each added one appended. Only move construction is
   used, so no running service is stopped: `Process`'s move assignment would stop what its target runs. Then the
   document, the origins, `Mode` and `LogLevel` are the new ones, and the log level takes effect.
5. **Answering:** the counts go back in a `ReloadReport`, and the log names each service.

A deferred definition replaces the service's definition at its next launch, whatever starts it, and sets the
health report's `restartToApply` until then. It brings its own work mode, except at a launch a restart command
asked for, which keeps the Auto the command set; a start or restart command for a service that is not running
takes the definition before it sets Auto. `OverrideStartupArgs` also changes the waiting definition, so a start
with other arguments keeps them.

## Logging

`Logger` is printf-style and thread-safe, with two outputs:
- **stderr:** with journald priority prefixes (`<3>` …) when stderr is the journal, and with colours on a
  terminal.
- **A file:** `tepegoz_<date>_<time>.log`, continued in a new file at 100 MB.

A forked child never logs, because it may hold no lock between `fork` and `exec`.

## Monitor GUI

| Module | Role |
|---|---|
| `goz.py` | The main window, the worker threads, and the command-line entry point. |
| `process_views.py` | Sidebar, service page (tiles, state band, tabs), manager page. |
| `network_view.py`, `network_status.py` | The broker page, and parsing of the broker's JSON. |
| `usage_graphs.py` | Usage history (15 minutes) and the graphs. |
| `journal_view.py` | Log text view: one colour per pid, new lines appended in place. |
| `remote_journal.py` | Client of the journal server: follows a journal with cursors and lists the cgroups' processes. For an older manager on the GUI's machine it falls back to local `journalctl` and `/sys/fs/cgroup`. |
| `log_recorder.py`, `record_view.py` | Recording services' logs into files on the GUI's machine, and the **Record logs** dialog. |
| `systemd_logs.py` | Local `journalctl`, `systemctl` and cgroup reading, and the journal JSON parser. |
| `health_structs.py` | The binary layouts of reports and commands. |
| `service_registry.py` | Client of the service registry, and the logic of the service dialog: requests, answers, the form's fields and what Edit keeps. No Qt. |
| `registry_view.py` | The service dialog: Add, Edit and Duplicate. |
| `ui_scale.py` | Font and window sizes that follow the screen. |
| `window_frame.py` | The main window without the system title bar: the toolbar's row is the title bar, with the window buttons, and the window's border resizes it. |
| `mock_publisher.py` | A stand-in manager for working on the GUI, its service registry included. |

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
| `RegistryWorker` | The service registry on 6671: one request at a time, from a queue | `list` every 5 s and after each change; the dialogs' requests as they come |

Each report holds every service, so an older one has nothing a newer one lacks. `ZmqWorker` reads every report
waiting on its socket and keeps the last, and it signals the window only after the window has taken the previous
one. A window that is busy then skips reports instead of falling further behind.

The window computes CPU percent from the difference between two reports. It marks the feed stale when reports
stop, and keeps a 15-minute history for the state band and the graphs.

The service registry has a worker of its own, so a registry that does not answer never holds up the window:
- **Requests:** the window queues each request with `RegistryWorker.request` and gets the answer as
  `reply_ready` with the request's id. Every request is answered once: by the registry, or by the GUI's own
  `unreachable` status when no connection comes within 3 s or no answer within 10 s.
- **One socket:** `RegistryClient` keeps one DEALER with `ZMQ_IMMEDIATE`, so a send fails at once instead of
  waiting for a registry that is not there. A request left without an answer closes the socket, so a late answer
  is never taken for the next request's.
- **What is registered:** a `list` every 5 s, and one after each change. It gives each service's origin and
  file, which decide which actions are open (`registry_actions`): Edit and Remove for registered services only,
  Duplicate for any service with a definition. Without an answer, every action is off, with a tooltip that says
  why.
- **The mark:** "restart to apply" comes from the health report's `restartToApply`, which is live, not from the
  list.
- **Edit keeps what the form does not show:** `build_definition` starts from the definition `show` returned,
  takes out only the keys the form covers, and puts back the ones the user set. A covered key whose field still
  says what the definition says keeps the value as written (`Args` as a text, the old spelling
  `EnviromentVariables`, a value the form would not take), so an edit that changes nothing does not mark the
  service changed.

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
- **`packaging/`:** installs the configuration (keeping an existing one), an empty `services.d` next to it
  (keeping an existing one), the log directory, the systemd unit and the GUI. Last, it enables the unit when that is possible.

## Tests

| Suite | Tests | Covers |
|---|---|---|
| `config_section_tests` | 26 | YAML reading, defaults, problems, renamed keys |
| `config_source_tests` | 41 | The main file with `services.d`: order, skipped and ignored files, origins, reading again, the folder's state, whether the main file takes services, and files overridden for a check |
| `service_registry_tests` | 43 | Registry requests and their parameters, names, editing a definition, YAML that reads back the same, JSON, problems a change adds, writing, removing and restoring files |
| `reload_plan_tests` | 33 | The reload plan: added, removed, changed, kept, unchanged and skipped services, what counts as a change, and the refusals |
| `tepegoz_tests` | 54 | Services built from a main file with `services.d`: the `Mode` default, `Enabled`, dependencies both ways, key checks, `--start all`; the reload of the manager's services, and every registry request, without its sockets |
| `process_tests` | 97 | One service run against the TestServices programs: configuration, restarts, stops, leftovers, environment, output, health, a deferred definition |
| `logger_tests` | 13 | Levels, files, rotation, journald prefixes |
| `network_manager_tests` | 35 | Routing, identities, status and events |
| `gpu_monitor_tests` | 27 | NVML stand-in: sampling, process lists, stuck calls |
| `journal_server_tests` | 42 | Argument building, the request protocol, stand-in `journalctl` scripts, cgroup listing |
| `test_service_tests` | 10 | The TestServices helpers |
| `status_table_tests` | 22 | The `status` table: columns, CPU figures, header, colours, the restart-to-apply mark |
| `command_watch_tests` | 17 | Following start, stop, restart and disable, for one service and for all |
| `status_client_tests` | 27 | Report decoding, `status --once`, commands and `reload` against a stand-in manager |
| `registry_client_tests` | 96 | The registry commands: finding the command, reading `Key=Value`, relative programs, every request, the wording of answers and the `list` table, `show` read back by `replace --from`, exit codes, and a stand-in registry that answers, stays silent or cannot be reached |

`ctest` runs all of them. Two more checks run outside ctest:
- `TestServices/scripts/run_scenarios.py` runs the real manager against twelve scenario configurations, covering
  start order, restarts, stopping, health checks, process trees, output, environment, resource figures, a
  service registered in `services.d`, reloads, and registering through the registry, by hand and with the
  command line's commands. It needs the manager's
  fixed ports, so ctest does not run it.
- The GUI's registry logic, its worker and its service dialog have a unittest module,
  `ProcessManagerGui/tests/test_service_registry.py`. It runs the worker and the window offscreen against
  `mock_publisher.py`'s registry on ports the system picks:
  `QT_QPA_PLATFORM=offscreen python3 -m unittest discover -s ProcessManagerGui/tests`. The rest of the GUI is
  checked against `mock_publisher.py` or a running manager.

## Known limits

- **No authentication:** the ports listen on every interface, except the registry's, which listens on 127.0.0.1
  unless `RemoteRegistration: true`. Whoever can send to the registry can make the manager run any program as
  root. Use a firewall.
- **Registered files are rewritten:** `replace` and `update` write a service's file from its keys and values, so
  the comments and the layout of a file written by hand are lost.
- **Fixed ports:** they are compile-time constants, so one manager per machine.
- **GUIs share one identity:** when two GUIs send commands at the same moment, an answer can reach the other
  GUI.
- **A reload compares text, not effect:** a definition is changed when its keys or values differ, even when the
  service would run the same (`Enabled: true` added, `Args` as a list instead of a text).
- **A reverted edit still waits:** a definition changed and then changed back before the service restarted is
  compared with the waiting one, not the running one, so the mark stays until the next start.
- **Kept services are not in the kept document:** after a reload kept a service whose new definition could not
  be read, the document the manager keeps has no usable entry for it. `--start all` then removes it, and a later
  reload that can read it again counts it as changed.
- **A reload names only what the reports show:** the command line names the added, removed and marked
  services; the kept and skipped ones are only counted, and the log names them.
- **Dependencies only order starts:** stopping or losing a service does not stop the services that depend on it,
  and `--start` starts only direct dependencies.
- **Heartbeats count from the start:** a service that sends no heartbeat within the longer of `GracePeriodMs` and
  `MissedHeartbeatThreshold × HeartbeatPeriodMs` after its start is restarted before it beats.
- **Grace periods hold up the start order:** while one service is in its grace period, the services after it
  wait.
