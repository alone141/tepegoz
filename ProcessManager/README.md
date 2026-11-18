# Tepegöz Release Notes

How to use and how it works: [quick start](../docs/QUICKSTART.md), [usage guide](../docs/USAGE.md),
[architecture](../docs/ARCHITECTURE.md).

### Build Requirements

#### Linux Dev Env.
gcc11, cmake 3.30. At least CMake 3.20, a C++20 compiler (GCC 10 or newer) and a C compiler, for libzmq.

#### Dependencies
All vendored in `third_party/` (see its README) and built with the project, so an airgapped machine needs no
package installed for them and nothing is downloaded:
- libzmq 4.3.5 (MPL-2.0), linked statically: the machine that runs the manager needs no ZeroMQ library.
- cppzmq 4.10.0 (MIT, header-only).
- cxxopts 3.3.1 (MIT, header-only).
- fkYAML 0.4.4 (MIT, header-only).
- GoogleTest 1.18.0 (BSD-3-Clause), only for the tests. `-DENABLE_TEST=OFF` builds without them.

#### Build and install
```text
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
ctest --test-dir build
sudo cmake --install build
sudo systemctl start tepegoz
```
`cmake --install` sets up everything the manager needs on the machine:
- **The manager:** installed in `bin/` as `tepegoz`.
- **The configuration:** `tepegoz.yaml` goes in the config directory unless one is already there.
  An existing file, which may have been edited, is kept, and the new one is put next to it as
  `tepegoz.yaml.new`. The share directory gets the same file, which the manager reads when the
  config directory has none. By default this is a starter configuration with no services;
  `-DTEPEGOZ_CONFIG_SOURCE=FILE` installs your own file instead. Next to it goes an empty `services.d` folder for
  registered services; an existing one is kept with its files.
- **The log directory:** `TepegozLog`, for the log files.
- **The systemd unit:** `tepegoz.service` runs the manager with `Delegate=yes`, which its cgroups
  need. Installing as root on a running systemd also enables the unit, so the manager starts at boot. It is not
  started at install time. A staged install (`DESTDIR`) only installs the file.
- **The monitor GUI:** installed in `share/tepegoz/gui/`, with its launcher `goz` in `bin/`. It
  needs Python 3 with PyQt6 and pyzmq, which the install does not provide.
- **The test setup (when the tests are built):** `keepalive` and the `tpz_*` test programs next to the manager,
  and their scenario configs and driver in `share/tepegoz/test-setup/` (see `TestServices/README.md`).

The paths are fixed when the build is configured and follow `CMAKE_INSTALL_PREFIX`. With `/usr`:
- the config is `/etc/tepegoz/tepegoz.yaml`;
- the logs go to `/var/log/tepegoz/TepegozLog/`;
- the unit goes in `/usr/lib/systemd/system/`.

Set the prefix when you configure, not with `cmake --install --prefix`, since the paths are already built into
the manager and the unit. Each path can also be set on its own; directories end with a slash.

| Cache variable | Default | Used for |
|---|---|---|
| `TEPEGOZ_CONFIG_DIR` | `<prefix>/etc/tepegoz/` | `tepegoz.yaml` |
| `TEPEGOZ_LOG_DIR` | `<prefix>/var/log/tepegoz/` | the `TepegozLog` directory |
| `TEPEGOZ_SHARE_DIR` | `<prefix>/share/tepegoz/` | the `tepegoz.yaml` read when the first is missing, and the GUI |
| `TEPEGOZ_BIN_INSTALL_DIR` | `bin` | where `cmake --install` puts the programs |
| `TEPEGOZ_CONFIG_SOURCE` | empty: a starter with no services | the `tepegoz.yaml` that is installed |
| `TEPEGOZ_SYSTEMD_UNIT_DIR` | `<prefix>/lib/systemd/system` | `tepegoz.service` |
| `TEPEGOZ_INSTALL_GUI` | `ON` | installing the GUI and `goz` |
| `TEPEGOZ_ENABLE_SERVICE` | `ON` | enabling the unit when installing as root on a running systemd |

The version the manager logs at start is the one in the root `project()` call.

### Configuration
`tepegoz.yaml`, written by `tepegoz generate` with every key documented.
`tepegoz validate --config FILE` reports YAML errors with their line, unknown keys,
values of the wrong kind, reserved names, missing dependencies and cycles.

### Unreleased
**ProcessManager is now called Tepegöz**, and every name it had changes with it. Only the new names are read,
so a machine that runs an earlier version needs the steps below before it runs this one.

| What | Before | Now |
|---|---|---|
| The program | `iltaren-ProcessManager` | `tepegoz` |
| The systemd unit | `processmanager.service` | `tepegoz.service` |
| The configuration | `/etc/processmanager/ProcessManagerConfig.yaml` | `/etc/tepegoz/tepegoz.yaml` |
| The manager's own settings in it | the `ProcessManager:` section | the `Tepegoz:` section |
| Registered services | `/etc/processmanager/services.d/` | `/etc/tepegoz/services.d/` |
| The share directory: fallback configuration, GUI, test setup | `/usr/share/processmanager/` | `/usr/share/tepegoz/` |
| The log files | `/var/log/processmanager/ProcessManagerLog/PMLog_*.log` | `/var/log/tepegoz/TepegozLog/tepegoz_*.log` |
| The CMake cache variables | `PM_CONFIG_DIR` and the other `PM_*` | `TEPEGOZ_CONFIG_DIR` and the other `TEPEGOZ_*` |
| The monitor GUI's title | `Process Monitor` | `Göz` |
| Its launcher | `process-monitor` | `goz` |
| Its main file | `process_monitor_gui.py` | `goz.py` |
| The manager's row in its sidebar | `processmanager` | `tepegoz` |
| Its default folder for recorded logs | `~/ProcessMonitorLogs` | `~/GozLogs` |
| The test programs, when the tests are built | `pmt_steady` and the other `pmt_*` | `tpz_steady` and the other `tpz_*` |

A configuration that still has a `ProcessManager:` section is reported as having an unknown key, and its
settings are not read. Configuring the build with a `PM_*` variable stops and names the new one;
`cmake -U 'PM_*' build` removes them from an older build folder. `cmake --install` does not enable
`tepegoz.service` while `processmanager.service` is enabled, since two managers would fight over the same ports;
it says how to switch.

On a machine that runs an earlier version, as root (the paths are those of the prefix `/usr`):
```text
systemctl disable --now processmanager.service
mkdir -p /etc/tepegoz
mv /etc/processmanager/ProcessManagerConfig.yaml /etc/tepegoz/tepegoz.yaml
mv /etc/processmanager/services.d /etc/tepegoz/
sed -i 's/^ProcessManager:/Tepegoz:/' /etc/tepegoz/tepegoz.yaml
cmake --install build                      # the new build: keeps the moved files, enables tepegoz.service
tepegoz validate --config /etc/tepegoz/tepegoz.yaml
systemctl start tepegoz
```
Then remove what the earlier version installed: `/usr/bin/iltaren-ProcessManager`, `/usr/bin/process-monitor`,
`/usr/lib/systemd/system/processmanager.service` (then `systemctl daemon-reload`), `/etc/processmanager/`,
`/usr/share/processmanager/`, the test programs `/usr/bin/pmt_*` if they were installed, and, once its logs
are no longer needed, `/var/log/processmanager/`. Scripts that call `iltaren-ProcessManager` or
`systemctl ... processmanager` need the new names.

- Services can be registered one per file, in the `services.d` folder next to the configuration file the manager
  reads. Each `<ProcessName>.yaml` holds the keys of one `Services` entry. The files are read in file name order,
  and their services come after the main file's; otherwise they are ordinary services. Subfolders, dot files and
  other extensions are ignored, and a missing folder is no problem. A file that is not valid YAML, is not a
  mapping, has a `ProcessName` other than its file name, or names a service the main file already has is
  reported by `validate` and at start-up, and skipped. `validate`, `graph` and `--start all` include the
  registered services. `cmake --install` creates the empty folder next to the installed configuration and keeps
  an existing one. The folder is read at start-up and on a reload, like the main file. The new module
  `ConfigSource/` reads both and keeps where each service was defined.
- `tepegoz reload`, and `systemctl reload tepegoz` through the unit's new `ExecReload`,
  make the running manager read its configuration file and `services.d` again and touch only what changed. A new
  service is added and started by its work mode; a service with no entry and no file, or with `Enabled: false`,
  is stopped and removed; a service whose new definition cannot be read is kept as it is; a changed service keeps
  running, and its new definition takes effect at its next start, whatever starts it; an unchanged one is left
  alone. Comments, key order and layout are no change. A main file that cannot be opened or parsed, has no
  `Services` list, a `services.d` that cannot be listed, a dependency cycle or two services of one name refuse the
  reload, and nothing changes. `LogLevel` takes effect at once, and a service removed with `--disable` comes back.
  The command prints what was done and exits with 0 when applied, 1 when refused, 2 without an answer within
  10 s; `--no-wait` only sends. A service whose new definition waits is marked: a byte that was padding in the
  144-byte health report (offset 89, `restartToApply`) carries the mark, and `status` shows `*` after its name.
  The reload is command 83 and is answered with a 16-byte `ReloadReport`. The new module `ReloadPlan/` compares
  the two configurations, and `ConfigSource` now tells an empty `services.d` from one it could not list.
- An exit whose status was lost, because something else in the manager's process reaped it, no longer counts
  as a failure. `OnFailure` used to restart such a service even when it had ended cleanly; `Always` and
  `UnlessStopped` still restart it.
- `UnlessStopped` no longer behaves exactly like `Always` inside `Process`: after `Stop()`, `NeedsRestart()`
  says no until the next `StartProcess()`, also when the stop came while a restart was waiting for its delay.
  The manager already kept a stopped service stopped in every mode, since a stop command also makes the
  service Manual, so what it does is unchanged.
- The defaults in `RestartPolicy.hpp` and `StdioPolicy.hpp` now match the configuration reader and the
  documentation: `MaxRestarts` 5 (was 10), `StdioAppend` false (was true) and `StdioFilePath`
  `/tmp/output.txt` (was empty). Services read from the configuration always got the documented values, so
  the manager behaves as before.
- `validate` and the start-up check no longer repeat a problem with `ExecutablePath` as `ResolvedPath does
  not exists` or `ResolvedPath is not a regular file`. Those two checks looked at `ExecutablePath` a second
  time. Checking the resolved path instead would not help: the checks follow symlinks, so it gives the same
  answers. They are gone.
- An exit during a stop the manager began no longer counts as a failure when SIGKILL ended it, as systemd
  treats a unit its stop job ended. A stop for missed heartbeats still does. The manager already kept a
  stopped service stopped, so this shows only inside `Process`. The SIGKILL after `ForceKillTimeoutMs` is
  now logged as a warning (`Graceful shutdown failed`) instead of a debug line.
- When a service's main process exits by itself, the manager now stops what it left behind. Its process group
  gets SIGTERM, then SIGKILL after `ForceKillTimeoutMs`, and whatever is left in its cgroup is killed. Such
  children used to run on beside the next instance, and a stop could no longer reach them. A restart, or a
  start command, waits until they are gone.
- The keys `EnviromentVariables` and `InheritParentEnviroment` are now spelled `EnvironmentVariables` and
  `InheritParentEnvironment`. The old spellings still work, but `validate` and the start-up check report them,
  so `validate` exits with 1 for a configuration that uses them until it is renamed. A service that sets both
  spellings of one key is ignored. `generate` writes the new spellings.
- The manager registers, changes and removes services on request, on port 6671 (the service registry, new
  module `ServiceRegistry/`). A request to `add`, `replace`, `update` or `remove` a service is checked as
  `validate` checks a configuration, written as one file in `services.d`, and applied with a reload; a request
  that would add a problem writes nothing. Only registered services can be changed this way; `show` and `list`
  cover every service. Requests are a YAML or JSON mapping, and replies one JSON object, so the GUI needs no
  YAML reader. The port listens on 127.0.0.1 only, unless the new `Tepegoz` key `RemoteRegistration` is
  true. It has no authentication, so whoever reaches it can make the manager run any program. `tpz_request`,
  a new test program, sends requests by hand.
- The command line registers services through the running manager: `add NAME EXECUTABLE [ARG...]` (arguments
  that start with `-` after `--`), `add NAME --from FILE`, `set NAME Key=Value... [--unset Key]`,
  `replace NAME --from FILE`, `remove NAME`, `show NAME` and `list`, with `--set`, `--no-start`, `--restart`,
  `--force`, `--dry-run`, `--json` and `--registry HOST`. A value in `Key=Value` is read as YAML when it is a
  number, a boolean, null, a quoted text, a list or a mapping, and is otherwise the text as typed; a value that
  cannot be read is refused. A relative `EXECUTABLE` is made absolute from the current folder. `show` prints only
  the YAML on standard output, so `show NAME > FILE`, an edit and `replace NAME --from FILE` work. The commands
  exit with 0 when done, 1 when refused, not understood or not reachable, and 2 without an answer within 10 s.
  The new module `RegistryClient/` builds the requests and words the answers.
- The monitor GUI registers, changes and removes services through the service registry. **Add service…** sits
  under the sidebar; **Edit…**, **Duplicate…** and **Remove…** are on a service's page and in its sidebar menu.
  Edit and Remove are for registered services; Duplicate copies any service. The service dialog has the name,
  executable, arguments (quoted like a shell's), working folder and "start it now" up front, and the rest folded
  under **Advanced**; only the fields you fill in are written, and **Check** shows the file the manager would
  write, or its problems next to the fields. Edit keeps every key the dialog does not show. Remove names the file
  it deletes, and when other services depend on the service it lists them and offers **Remove anyway**. A service
  waiting for a restart is marked `*` in the sidebar and **restart to apply** on its page, which also says where
  the service is defined. New option `--registry ENDPOINT`; `--host` and the HOST field set it too. Without a
  registry that answers, these actions are greyed out with the reason. `mock_publisher.py` answers the registry
  as well, and the GUI has its first tests, `ProcessManagerGui/tests/test_service_registry.py`.

### 0.2.0
- The configuration is YAML: a `Services` list with the keys of the former INI sections, any number of
  services (the INI format stopped at 10), and `Enabled: false` instead of renaming a section to `[x]`.
  INI files need converting.
- The two legacy libraries are no longer used: the manager has its own logger (console with journald
  priorities, files rotated at 100 MB) and its own configuration reader. `[ProcessManager] LogLevel` sets the level.
- The manager samples each service's GPU use through NVML, loaded at run time, and sends it in the health
  reports, which grow from 128 to 144 bytes. The monitor GUI reads it from there instead of running nvidia-smi.
- The ZeroMQ broker that services, the CLI and the GUI talk through is part of this tree (`NetworkManager/`)
  instead of coming from the build that adds it. It still listens on `tcp://*:5557` and hands each message
  to the identity it names.
- The broker's log lines start with `Broker:`, a mark and a name column:
  `+` a service joined (from which address), `-` it left (after how long, with how many messages),
  `~` another connection took its name, `!` a message was not delivered or was rejected (a repeat is
  written once per 10 s, with the count held back), and `=` a status line every minute: who is connected
  and how many messages each sent. Forwarded messages are logged at `LogLevel: Verbose`.
- On a terminal, warnings are yellow, errors red and debug lines dim. journald and the log files get plain
  text, and `NO_COLOR` turns the colours off.
- The broker publishes its status on `tcp://*:6669` (ZeroMQ PUB, versioned JSON): every second who is
  connected, from where, since when and how many messages each sent, and every event of its log as it
  happens. The monitor GUI shows it as the "network manager" page under the manager: a table of the
  connected dealers, the events, and a toolbar pill that turns red when a service that sends heartbeats is
  not connected or the status stops. `--network` points the GUI elsewhere.
- The tree is a project of its own instead of a directory that another build adds. Its root `CMakeLists.txt`
  sets C++20, builds libzmq 4.3.5, cppzmq 4.10.0, cxxopts 3.3.1 and GoogleTest 1.18.0 from `third_party/`
  (libzmq statically, as before), and sets the config, log and share paths, the install directory and the
  version, which that build used to provide.
- The manager serves its journal on `tcp://*:6670` (`JournalServer/`), so the monitor GUI shows the logs
  when it runs on another machine. The GUI asks for the manager's, a service's or a process's journal, and
  the manager runs journalctl (or systemctl status) on its own machine and sends back what it prints. After
  the first read, the GUI asks only for the lines after the last one it has, so no line is lost between
  refreshes. `--host H` points every GUI endpoint at the manager's machine, and `--journal` points the
  journal elsewhere. A manager from before this has no journal server; on the GUI's own machine, the GUI
  then runs journalctl itself, as before.
- The journal server also lists the processes in each service's cgroup (`["members"]`, protocol version 2),
  so the GUI's cgroup PIDs tab shows them when the GUI runs on another machine; it used to read the GUI's
  own machine and stayed empty there. A line above the table says why it is empty: the service is not
  running, it runs without a cgroup, or the journal server does not answer.
- The health reports on 6667 go out every 500 ms, and at once when a service
  starts, stops, restarts, or is added or removed. They used to go out on every pass of the supervision loop,
  and every heartbeat starts a pass, so each subscriber's traffic grew with the square of the number of
  services: 50 services beating every 100 ms sent 186 reports, 1.3 MiB, a second to each GUI, and now send 2
  reports, 14 KiB. The monitor GUI now shows only the newest report waiting, so a busy GUI skips reports
  instead of falling behind.
- The GUI's Connection strip holds only a HOST field: a machine typed there goes into all four endpoints,
  each keeping its port, and the GUI reconnects, as `--host` does at start. The window no longer shows or
  edits the endpoints themselves; the toolbar shows the machine, and `--sub`, `--dealer`, `--network` and
  `--journal` still set an endpoint at start. Closing the GUI right after a reconnect no longer hangs for
  8 s and then kills its own worker threads.
- `status` draws a new table: columns that fit the names, uptime, heartbeat age or missed beats, GPU % and
  VRAM, coloured states, and a header line with the machine, the count of each state, the GPU state and the
  time, which says `No report for N s` when the reports stop. Unhealthy services show their CPU and memory,
  which read 0 before, and the CPU column is headed `CPU %`. `status --once` prints one table and exits.
- `--start`, `--kill`, `--restart` and `--disable` refuse a name the manager does not have, follow the reports
  for up to 10 s and print what happened to each service. They exit with 0 when done, 1 when it failed and 2
  when it is not done within 10 s. `--no-wait` only sends the command, as before.
- `TestServices/` holds test programs with planned behaviours (clean and failing exits, crashes, ignored
  SIGTERM, heartbeats, children, output, environment, CPU and memory use), scenario configs that run them,
  and `run_scenarios.py`, which runs the manager with each config and checks what it does.
- `cmake --install` sets up the whole machine (`packaging/`). Besides the programs, it installs:
  - the configuration, keeping an existing one;
  - the log directory;
  - the systemd unit `processmanager.service`, enabled when installing as root;
  - the monitor GUI with its `process-monitor` launcher.
- The 4-byte short status on port 6668 and the version reply with one slot per sibling system are gone:
  both served one earlier consumer. `GetVersionInfo` answers with the manager's own version, and the
  manager binds 6667 only.

### 0.1.0 
- First release
