# Bihter: Process Manager – Release Notes

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
sudo systemctl start processmanager
```
`cmake --install` sets up everything the manager needs on the machine:
- **The manager:** installed in `bin/` as `iltaren-ProcessManager`.
- **The configuration:** `ProcessManagerConfig.yaml` goes in the config directory unless one is already there.
  An existing file, which may have been edited, is kept, and the new one is put next to it as
  `ProcessManagerConfig.yaml.new`. The share directory gets the same file, which the manager reads when the
  config directory has none. By default this is a starter configuration with no services;
  `-DPM_CONFIG_SOURCE=FILE` installs your own file instead.
- **The log directory:** `ProcessManagerLog`, for the log files.
- **The systemd unit:** `processmanager.service` runs the manager with `Delegate=yes`, which its cgroups
  need. Installing as root on a running systemd also enables the unit, so the manager starts at boot. It is not
  started at install time. A staged install (`DESTDIR`) only installs the file.
- **The monitor GUI:** installed in `share/processmanager/gui/`, with its launcher `process-monitor` in `bin/`. It
  needs Python 3 with PyQt6 and pyzmq, which the install does not provide.
- **The test setup (when the tests are built):** `keepalive` and the `pmt_*` test programs next to the manager,
  and their scenario configs and driver in `share/processmanager/test-setup/` (see `TestServices/README.md`).

The paths are fixed when the build is configured and follow `CMAKE_INSTALL_PREFIX`. With `/usr`:
- the config is `/etc/processmanager/ProcessManagerConfig.yaml`;
- the logs go to `/var/log/processmanager/ProcessManagerLog/`;
- the unit goes in `/usr/lib/systemd/system/`.

Set the prefix when you configure, not with `cmake --install --prefix`, since the paths are already built into
the manager and the unit. Each path can also be set on its own; directories end with a slash.

| Cache variable | Default | Used for |
|---|---|---|
| `PM_CONFIG_DIR` | `<prefix>/etc/processmanager/` | `ProcessManagerConfig.yaml` |
| `PM_LOG_DIR` | `<prefix>/var/log/processmanager/` | the `ProcessManagerLog` directory |
| `PM_SHARE_DIR` | `<prefix>/share/processmanager/` | the `ProcessManagerConfig.yaml` read when the first is missing, and the GUI |
| `PM_BIN_INSTALL_DIR` | `bin` | where `cmake --install` puts the programs |
| `PM_CONFIG_SOURCE` | empty: a starter with no services | the `ProcessManagerConfig.yaml` that is installed |
| `PM_SYSTEMD_UNIT_DIR` | `<prefix>/lib/systemd/system` | `processmanager.service` |
| `PM_INSTALL_GUI` | `ON` | installing the GUI and `process-monitor` |
| `PM_ENABLE_SERVICE` | `ON` | enabling the unit when installing as root on a running systemd |

The version the manager logs at start is the one in the root `project()` call.

### Configuration
`ProcessManagerConfig.yaml`, written by `iltaren-ProcessManager generate` with every key documented.
`iltaren-ProcessManager validate --config FILE` reports YAML errors with their line, unknown keys,
values of the wrong kind, reserved names, missing dependencies and cycles.

### Unreleased
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
- The health reports on 6667, and the short status on 6668, go out every 500 ms, and at once when a service
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
- `TestServices/` holds test programs with planned behaviours (clean and failing exits, crashes, ignored
  SIGTERM, heartbeats, children, output, environment, CPU and memory use), scenario configs that run them,
  and `run_scenarios.py`, which runs the manager with each config and checks what it does.
- `cmake --install` sets up the whole machine (`packaging/`). Besides the programs, it installs:
  - the configuration, keeping an existing one;
  - the log directory;
  - the systemd unit `processmanager.service`, enabled when installing as root;
  - the monitor GUI with its `process-monitor` launcher.

### 0.1.0 
- First release
