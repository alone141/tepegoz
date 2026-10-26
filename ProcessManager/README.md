# ProcessManager Release Notes

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

#### Build
```text
cmake -S . -B build
cmake --build build
ctest --test-dir build
cmake --install build
```
`cmake --install` puts the manager in `bin/` as `iltaren-ProcessManager`, and `keepalive` next to it when
the tests are built. The paths below are built into the manager. They follow `CMAKE_INSTALL_PREFIX`, so with
`-DCMAKE_INSTALL_PREFIX=/usr` the config is `/etc/processmanager/ProcessManagerConfig.yaml` and the logs go to
`/var/log/processmanager/ProcessManagerLog/`. Each can also be set on its own; directories end with a slash.

| Cache variable | Default | Used for |
|---|---|---|
| `PM_CONFIG_DIR` | `<prefix>/etc/processmanager/` | `ProcessManagerConfig.yaml` |
| `PM_LOG_DIR` | `<prefix>/var/log/processmanager/` | the `ProcessManagerLog` directory |
| `PM_SHARE_DIR` | `<prefix>/share/processmanager/` | the `ProcessManagerConfig.yaml` read when the first is missing |
| `PM_BIN_INSTALL_DIR` | `bin` | where `cmake --install` puts the programs |

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

### 0.1.0 
- First release
