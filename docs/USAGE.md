# Usage guide

The complete reference for running Tepegöz, the process manager, and its monitor GUI, Göz. For the short version, see
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
cmake -S manager -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j
ctest --test-dir build
sudo cmake --install build
```

The build needs CMake 3.20 or newer and GCC 10 or newer. libzmq, cppzmq, cxxopts, fkYAML and GoogleTest come
with the source in `manager/third_party/`, so nothing is downloaded and the target machine needs no
ZeroMQ library.

`cmake --install` installs:

| Item | Path with prefix `/usr` | Notes |
|---|---|---|
| Manager | `/usr/bin/tepegoz` | |
| Configuration | `/etc/tepegoz/tepegoz.yaml` | Only if missing. An existing file is kept; if the new one differs, it is put next to it as `tepegoz.yaml.new`. |
| Registered services | `/etc/tepegoz/services.d/` | Made empty if missing. An existing folder is kept with its files. See [Registered services](#registered-services-servicesd). |
| Fallback configuration | `/usr/share/tepegoz/tepegoz.yaml` | Read when the file above is missing. |
| Log directory | `/var/log/tepegoz/TepegozLog/` | |
| systemd unit | `/usr/lib/systemd/system/tepegoz.service` | Enabled when installed as root on a running systemd. Not started. |
| Monitor GUI | `/usr/share/tepegoz/gui/`, launcher `/usr/bin/goz` | |
| Menu entry | `/usr/share/applications/goz.desktop`, icon `/usr/share/icons/hicolor/scalable/apps/goz.svg` | Göz in the desktop's application menu. |
| Test programs (tests built) | `/usr/bin/keepalive`, `/usr/bin/tpz_*`, `/usr/share/tepegoz/test-setup/` | See [Testing with TestServices](#testing-with-testservices). |

The paths are built into the manager and the unit when you configure, so set the prefix then rather than with
`cmake --install --prefix`. These cache variables change the install:

| Variable | Default | Meaning |
|---|---|---|
| `CMAKE_INSTALL_PREFIX` | `/usr/local` | Base of every path below. |
| `TEPEGOZ_CONFIG_DIR` | `<sysconfdir>/tepegoz/`: `/etc/tepegoz/` for `/usr`, `/usr/local/etc/tepegoz/` for `/usr/local` | Where the manager reads `tepegoz.yaml`. |
| `TEPEGOZ_LOG_DIR` | `<localstatedir>/log/tepegoz/`: `/var/log/tepegoz/` for `/usr`, `/usr/local/var/log/tepegoz/` for `/usr/local` | Parent of the `TepegozLog` directory. |
| `TEPEGOZ_SHARE_DIR` | `<prefix>/share/tepegoz/` | Fallback configuration, GUI and test setup. |
| `TEPEGOZ_BIN_INSTALL_DIR` | `bin` | Where the programs go. |
| `TEPEGOZ_CONFIG_SOURCE` | empty | The configuration file to install. Empty installs a starter with no services. |
| `TEPEGOZ_SYSTEMD_UNIT_DIR` | `<prefix>/lib/systemd/system` | Where the unit goes. |
| `TEPEGOZ_INSTALL_GUI` | `ON` | Installs the GUI and its launcher. |
| `TEPEGOZ_ENABLE_SERVICE` | `ON` | Enables the unit when installing as root on a running systemd. |
| `ENABLE_TEST` | `ON` | Builds the tests and installs the test programs. |

## Running the manager

### As a systemd service

```bash
sudo systemctl start tepegoz
sudo systemctl stop tepegoz
sudo systemctl restart tepegoz          # stops every service and starts the manager again
sudo systemctl reload tepegoz           # reads a changed configuration without a restart
systemctl status tepegoz
journalctl -u tepegoz -f
```

The installed unit has these settings:
- **`Delegate=yes`:** the manager can create a cgroup for itself and for each service.
- **`KillMode=mixed`:** a stop sends SIGTERM to the manager alone, and the manager stops its services itself.
  Each service gets up to twice its `ForceKillTimeoutMs`.
- **`TimeoutStopSec=180`:** after 180 s, systemd kills whatever is still running.
- **`Restart=on-failure`:** systemd restarts the manager if it exits with an error or crashes. A manager that
  stops because its broker failed exits with 0, so it is not restarted.
- **`ExecReload`:** `systemctl reload` runs `tepegoz reload`. See
  [Reloading the configuration](#reloading-the-configuration). A refused reload makes `systemctl reload` fail.

### By hand

```bash
tepegoz                               # reads the installed configuration
tepegoz --config ./my.yaml            # reads this file, with no fallback
```

Without `--config`, the manager reads `TEPEGOZ_CONFIG_DIR/tepegoz.yaml`. If that file is missing, it
reads `TEPEGOZ_SHARE_DIR/tepegoz.yaml`. With `--config`, a missing file is an error. In every case it
also reads the `services.d` folder next to the file it read.

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
- **In files:** `tepegoz_<date>_<time>.log` in `TEPEGOZ_LOG_DIR/TepegozLog/`. A file is continued in a new one
  at 100 MB. If the directory cannot be created, the manager logs to the console only.
- **The level:** `LogLevel` in the configuration sets the lowest level that is logged.

Lines about one service start with its name in brackets, for example `[server] pid 1234 exited with code 1`.
Services in `Pipe` mode have their output logged the same way: `[server] <the line it printed>`.

Each start of the manager begins its log with an eye, 15 lines of Braille characters, and then its version:
`TEPEGOZ VERSION: 0.2.0`. A console without UTF-8 or a Braille font shows the eye's lines garbled.

## Command line

`tepegoz [COMMAND] [OPTIONS]` (named `TepegozApp` in a build tree).

| Command or option | What it does |
|---|---|
| *(nothing)* | Runs the manager with the installed configuration. |
| `--config FILE`, `-c FILE` | Runs the manager with `FILE`. Also names the file for `validate` and `graph`. |
| `validate --config FILE` | Checks `FILE` and the `services.d` folder next to it: YAML syntax with line numbers, unknown keys, wrong values, reserved names, missing executables and folders, missing dependencies, duplicates, cycles, and the rules for registered services. Exits 1 if it finds a problem. |
| `graph --config FILE` | Prints the order in which the services of `FILE` and its `services.d` start. |
| `generate` | Writes `tepegoz.yaml` with every key documented, in the current folder. |
| `status` | Shows a live table, redrawn every second. See [The status table](#the-status-table). Ctrl+C leaves. |
| `status --once` | Prints the table once and exits. It follows the reports for a second first, so CPU has a figure. Exits with 1 if no report comes within 3 s. |
| `reload` | The running manager reads its configuration and `services.d` again and applies what changed. See [Reloading the configuration](#reloading-the-configuration). |
| `reload --no-wait` | Sends the reload and exits at once, with 0, or with 1 when the manager is not reachable. |
| `add NAME EXECUTABLE [ARG...]` | Registers a service with the running manager, which writes `services.d/NAME.yaml` and starts it. See [Registering services through the manager](#registering-services-through-the-manager). |
| `add NAME --from FILE` | Registers a service from a YAML file with the keys of one service. |
| `set NAME Key=Value...` | Changes single keys of a registered service; `--unset Key` removes one. |
| `replace NAME --from FILE` | Gives a registered service a whole new definition. |
| `remove NAME` | Removes a registered service, which stops. |
| `show NAME` | Prints a service's definition as YAML; where it is defined and how it runs go to standard error. |
| `list` | A table of every service: name, origin, state, pid, mark and file. |
| `--start NAME`, `-s NAME` | Starts `NAME` and the services in its `DependsOn`. Sets it to Auto, so it is restarted by its `RestartMode`. |
| `--kill NAME`, `-k NAME` | Stops `NAME` and sets it to Manual, so it stays stopped. |
| `--restart NAME`, `-r NAME` | Stops `NAME` and starts it again, with its dependencies started first. Sets it to Auto. |
| `--disable NAME`, `-d NAME` | Stops `NAME` and removes it from the manager until the manager restarts, or until a reload finds it in the configuration again. |
| `--kill all` | Stops every service and sets each to Manual. |
| `--start all`, `--restart all` | Stops every service, rebuilds all of them from the configuration last read, at startup or by the last applied reload (registered services included, as they were then), and starts every one, Manual ones included. Disabled services come back, and every definition that waited for a restart takes effect. |
| `--help`, `-h` | Lists the options. |

- **Where commands go:** `--start`, `--kill`, `--restart`, `--disable` and `reload` go to the manager on the
  same machine, through `tcp://localhost:5557`. `add`, `set`, `replace`, `remove`, `show` and `list` go to
  `tcp://localhost:6671`, or to the manager `--registry` names.
- **Checking the name:** the command line reads the manager's reports first. A name the manager does not have
  is not sent; the message lists the names it has.
- **Following the command:** after sending, the command line follows the reports for up to 10 s and prints what
  happened to each service, for example `viewer: Running, pid 4321`, `viewer: was not running` or
  `viewer: started, then stopped again; its log says why`.
- **Exit codes:**
  - 0: every service got where the command sends it;
  - 1: one failed, the name is unknown or longer than 31 bytes, or the manager did not take the command
    within 2 s;
  - 2: a service was not there after 10 s, such as one still waiting for its first heartbeat.
- **`--no-wait`:** sends the command and exits at once, with 0, without checking the name or following it.
- **Wrong command lines:** an unknown option, an option without its value, or an unknown command exits with 1
  and a message.

### The status table

```text
target-01 | 4 services: 2 running, 1 unhealthy, 1 stopped | GPU ok | 14:02:31
NAME              STATE        PID    UPTIME  CPU %    MEMORY  GPU %      VRAM  HEARTBEAT  RESTARTS
control_loop      Running    10003  02:14:05   27.4  214.1 MB      0       0 B  0.1s ago          2
sensor_fusion     Unhealthy  10005     05:12    8.1  140.2 MB     21  594.0 MB  3 missed          3
telemetry_bridge  Running    10006     31:05   15.2   61.4 MB      0       0 B  -                 0
vision_pipeline   Stopped        -         -      -         -      -         -  -                 5
```

- **The header line:** the machine, how many services are in each state, the GPU state, and the time of the
  report. When reports stop, it ends with `No report for N s`.
- **CPU %:** percent of one core over the last second, so 200 is two busy cores. It shows `-` until two
  reports of the same process have come.
- **GPU % and VRAM:** shown only when the manager samples the GPUs.
- **HEARTBEAT:** how long ago the last heartbeat came, or the missed beats in red. It shows `-` for a service
  without `CheckHealth`, or before its first heartbeat.
- **Stopped services:** `-` for the process figures.
- **`*` after a name:** the service's new definition from a reload waits for its next start ("restart to
  apply"). A line under the table explains the mark when a service has it.
- **Colours:** the states are coloured on a terminal. `NO_COLOR`, `TERM=dumb` or output to a pipe turn them off.

## Configuration

The file is YAML with two parts: `Tepegoz` holds the manager's own settings, and `Services` lists the
programs to run. More services can be registered one per file, in the `services.d` folder next to it (see
[Registered services](#registered-services-servicesd)).

```yaml
Tepegoz:
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
- **Renamed keys:** `EnvironmentVariables` and `InheritParentEnvironment` were once spelled `EnviromentVariables`
  and `InheritParentEnviroment`. The old spellings still work, and `validate` and the startup check report them.
  A service that sets both spellings of one key is ignored.
- **Quoting:** a value with two spaces in a row must be quoted (`"a  b"`), because of the YAML reader in use.

### `Tepegoz`

| Key | Values | Default | Meaning |
|---|---|---|---|
| `Mode` | `Auto`, `Manual`, `Inherit` | `Inherit` | The `WorkMode` of every service that sets none. `Inherit`: such services are Manual. |
| `LogLevel` | `Verbose`, `Debug`, `Info`, `Warning`, `Error` | `Debug` | Lowest level that is logged. `Verbose` adds every message the broker forwards. |
| `RemoteRegistration` | `true`, `false` | `false` | Where services can be registered from through port 6671: `false`, this machine only; `true`, any machine that reaches the port. Read at startup only. See [Registering services through the manager](#registering-services-through-the-manager). |

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
| `InheritParentEnvironment` | `true` | `true`: start with the manager's environment. `false`: start with an empty one. |
| `EnvironmentVariables` | none | Variables to set: a mapping (`NAME: value`) or a list of `NAME=value` texts. |
| `UseDisplay` | `false` | Sets `DISPLAY` and `XAUTHORITY`, for a program that opens windows. |
| `Display` | `":1"` | The `DISPLAY` value, with `UseDisplay`. |
| `XAuthority` | `/home/user/.Xauthority` | The `XAUTHORITY` value, with `UseDisplay`. |
| `CreateProcessGroup` | `true` | Gives the service a cgroup of its own, for CPU and memory figures and a complete stop. |

### Registered services (`services.d`)

A service can also have a file of its own in the `services.d` folder next to the configuration file the manager
read: `/etc/tepegoz/services.d/` with prefix `/usr`, or the share directory's when the manager fell back to
the copy there. `cmake --install` makes the folder empty if it is missing, and keeps an existing one.

```yaml
# /etc/tepegoz/services.d/viewer.yaml
ProcessName: viewer
ExecutablePath: /opt/app/bin/viewer
DependsOn: [server]
```

- **One service per file:** the file is a mapping with the keys of one `Services` entry, and it is named
  `<ProcessName>.yaml`.
- **Order:** the files are read in file name order, and their services come after those of the main file.
  `DependsOn` works both ways between the two.
- **Otherwise an ordinary service:** `Enabled: false`, the `Mode` default and every check apply as in the main
  file.
- **Ignored without a word:** subfolders, names that start with a dot, and names that do not end in `.yaml`
  (`.YAML` included). A missing folder is no problem either.
- **Problems:** `validate` and the startup check report a file that is not valid YAML, is not a mapping, has a
  `ProcessName` other than its file name, or names a service the main file already has, enabled or not (the main
  file's service is used). A file that cannot be read is reported too. Such a file is skipped, and everything
  else is loaded.
- **When the folder is read:** at startup and on every reload, like the main file. `validate --config FILE` and
  `graph --config FILE` read the folder next to `FILE`.
- **Written by the manager too:** the manager writes and deletes these files itself when it is asked to register,
  change or remove a service. See [Registering services through the manager](#registering-services-through-the-manager).

### Reloading the configuration

`tepegoz reload` (or `systemctl reload tepegoz`) makes the running manager read its
configuration file and its `services.d` folder again, and touch only what changed. It works service by service,
by `ProcessName`:

| The service | What the reload does |
|---|---|
| New: in the configuration, not in the manager | Adds it and starts it by its work mode, as at startup, in dependency order. |
| Gone: no entry in the main file and no file in `services.d`, or `Enabled: false` | Stops it and removes it, as `--disable` does. |
| Present, but its new definition cannot be read | Keeps it exactly as it is, running or not, and logs the problem. |
| Changed | Keeps it running with its old definition. The new one waits for its next start, whatever starts it. |
| Unchanged | Leaves it alone: same pid, same restart count, same start time. |

- **What a change is:** the service's own keys and values. Comments, the order of the keys, the layout of the
  file and a move between the main file and `services.d` with the same keys are no change.
- **Cannot be read:** a file in `services.d` that is skipped for a problem (see
  [Registered services](#registered-services-servicesd)), or an entry the manager ignores, such as one without
  `ExecutablePath`. An entry whose `ProcessName` itself cannot be read could be any service, so while there is
  one, no service is removed.
- **Restart to apply:** a changed service is marked until its new definition takes effect, with `*` in
  `status`. The new definition takes effect at the service's next start: `--start`, `--restart`, an automatic
  restart, or `--start all`. Taking effect starts and stops nothing by itself, and a service stopped with
  `--kill` stays stopped. It brings its own `WorkMode`, except that `--start` and `--restart` still make the
  service Auto.
- **Refused as a whole:** the manager logs an error and changes nothing when the main file cannot be opened or
  is not valid YAML, its top level is not a mapping or its `Services` is not a list, the `services.d` folder
  exists but cannot be listed, or the new configuration has two services of the same name or a dependency
  cycle. A folder that cannot be listed is never taken for an empty one.
- **`Tepegoz` settings:** `LogLevel` takes effect at once. A changed `Mode` applies to services the
  reload adds from then on, and to new definitions when they take effect; running services keep their work mode.
- **Disabled services:** one removed with `--disable` comes back on a reload if the configuration still has it.
- **After the reload:** `--start all` builds from the configuration this reload read.
- **What the command prints:** whether the reload was applied or refused, how many services were added,
  removed, changed, kept and unchanged, and the names the health reports show: the added and removed ones, and
  the ones that wait for a restart. The manager's log names every service.
- **Exit codes:** 0 when applied, also when nothing changed; 1 when refused or the manager is not reachable;
  2 when no answer comes within 10 s. `--no-wait` only sends the reload, and the manager does not answer it: 0
  once it is sent, 1 when the manager is not reachable.

### Registering services through the manager

The manager also takes requests to register a service, to change or remove a registered one, and to show what it
has, on port 6671. For a change it checks the request, writes the service's file in `services.d`, and reloads. The
command line's commands below send these requests, and so does the GUI (see
[Registering services in the GUI](#registering-services-in-the-gui)); the
[architecture document](ARCHITECTURE.md#service-registry-port-6671) has the protocol, for other clients.

```bash
tepegoz add camera /opt/cam/bin/server -- --port 8080
tepegoz set camera MaxRestarts=3 Display=:1
tepegoz show camera > camera.yaml       # edit camera.yaml, then:
tepegoz replace camera --from camera.yaml --restart
tepegoz list
tepegoz remove camera
```

| Command | What the manager does |
|---|---|
| `add NAME EXECUTABLE [ARG...]` | Registers a new service: writes `services.d/NAME.yaml` and reloads, so the service starts by its work mode. The words after `EXECUTABLE` are its `Args`. With `--no-start` it is registered but held until `--start NAME`. |
| `add NAME --from FILE` | The same, with the keys of the service from a YAML file, such as one `show` printed. |
| `set NAME Key=Value...` | Sets single keys of a registered service; `--unset Key` removes one. |
| `replace NAME --from FILE` | Gives a registered service a whole new definition. |
| `remove NAME` | Deletes a registered service's file and reloads, which stops the service and removes it. |
| `show NAME` | Prints one service's definition as YAML. |
| `list` | Lists every service with where it is defined and how it runs. |

How the command line reads what you type:
- **Arguments of the program:** every word after `EXECUTABLE` is one argument. A word that starts with `-` would be
  taken for an option, so put such words after `--`: `add camera /opt/cam/bin/server -- --port 8080`.
- **More keys:** `add` takes `--set Key=Value`, as often as needed: `--set StdioMode=Pipe --set 'DependsOn=[server]'`.
  With `--from`, a `--set` key replaces the file's.
- **Values:** in `Key=Value` the value is read as YAML. `MaxRestarts=3` is a number, `CheckHealth=false` a
  boolean, `null` or `~` YAML's null, `'Args=[--port, 9090]'` a list and `'EnvironmentVariables={MODE: fast}'` a
  mapping. Any other value is the text exactly as typed, with its spaces, `#`, `:` and `=`: `Display=:1`,
  `'Cwd=/opt/x y'`, `'Title=a # b: c'`. Quotes make a text of anything: `'Display="1"'`. The command line refuses
  a value it cannot read rather than guess: an empty one (`Cwd=`; write `'Cwd=""'` for an empty text, or
  `--unset Cwd`), a quote that does not close at the end, and a list or mapping that does not end with its `]` or
  `}`. A key given twice is refused too.
- **Paths:** a relative `EXECUTABLE`, such as `./bin/server` or `bin/server`, is made absolute from the current
  folder before it is sent, and `--from FILE` is read from the current folder. A bare program name such as
  `sleep` is sent as it is, and the manager's check decides. Other paths, such as `Cwd`, are sent as typed.
- **The editing round trip:** `show NAME` prints exactly the YAML of the definition on standard output, and where
  it is defined and how it runs on standard error, so `show NAME > FILE`, an edit, and
  `replace NAME --from FILE` work. For a registered file that is skipped, `show` prints the file's text, so that
  it can be repaired the same way.
- **`--dry-run`:** `add`, `set`, `replace` and `remove` check the request and write nothing. `add`, `set` and
  `replace` print the YAML that would be written on standard output and the rest on standard error, so
  `add camera /opt/cam/bin/server --dry-run > camera.yaml` gives a file to edit and `add camera --from`.
- **The `list` table:** `NAME ORIGIN STATE PID MARK FILE`. `ORIGIN` is `main` or `registered`. `STATE` is the
  state the manager reports, `Disabled` for a service it does not have (`Enabled: false`, or `--disable`), and
  `Skipped` for a registered file it skipped. `*` in `MARK` means restart to apply; `!` means the file is skipped,
  and a line under the table gives the problem.
- **`--json`:** `show` and `list` print the manager's answer, one JSON object, as it came.
- **Another machine:** `--registry HOST`, `HOST:PORT` or `tcp://HOST:PORT` sends to a manager with
  `RemoteRegistration: true`. Paths are checked on the manager's machine.
- **What is printed:** the manager's message, such as `'camera' is registered and starts`, and the file. A
  refusal prints `Refused:` with the message on standard error, then each problem on a line of its own.
- **Exit codes:** 0 when done, a dry run included; 1 when refused, not understood, the command line is wrong, or
  the manager is not reachable; 2 when no answer comes within 10 s. These commands always wait for the answer.

What the manager does with a request:
- **Checked first, written only when valid:** the manager builds the configuration the request would make, in
  memory, and checks it as `validate` does, including the executable and the folders on its own machine. It
  refuses the request when that would add a problem: a missing executable, an unknown key, a dependency on a
  service that does not exist, a cycle. Problems the configuration has already, in other services, do not block
  a request. A refused request writes nothing.
- **Names:** a registered service's name is also its file name: 1 to 31 letters, digits, `_`, `-` and `.`, not
  starting with a dot, and not `pm`, `all`, `BPM`, `PMC` or `GUI`. A name the main file or `services.d` has
  already cannot be added.
- **Only registered services change:** services of the main file can be shown and listed. Changing or removing
  one needs an edit of the main file.
- **A changed service:** as after a reload, a running service keeps running and is marked "restart to apply".
  With `--restart` it restarts at once. A stopped service with `--restart` takes its new definition at once and
  stays stopped.
- **Removing a needed service:** refused while enabled services name it in `DependsOn`, unless `--force`. Those
  services keep running, and their next start misses the dependency.
- **The file:** written from the checked definition as plain YAML with `ProcessName` first, through a temporary
  dot file that is renamed into place. `replace` and `set` rewrite the file, so comments in a file written by
  hand are lost.
- **The whole configuration is reloaded:** a request also applies other changes made to the files, as `reload`
  would. If that reload is refused, the manager puts the file back as it was.
- **Where requests come from:** port 6671 listens on 127.0.0.1 only. With `RemoteRegistration: true` it
  listens on every interface. The port has no authentication and the manager runs as root, so whoever can send to
  it can make the manager run any program as root: with the default, any user of the manager's machine; with
  `RemoteRegistration: true`, any machine that reaches the port. Keep the default unless a firewall admits only
  trusted machines. The setting is read at startup only, and the manager logs where the port listens.

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
| `OnFailure` | A failure: an exit code other than 0, a crash by a signal other than SIGTERM, SIGINT, SIGHUP or SIGPIPE, or a stop for missed heartbeats. An exit whose status was lost is not a failure, and neither is an exit during any other stop the manager began, even one that SIGKILL ended. |
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
2. **SIGKILL:** after `ForceKillTimeoutMs`, it logs the warning `Graceful shutdown failed`, sends SIGKILL to
   the group and kills whatever is left in the service's cgroup.
3. **Giving up:** if processes remain after another `ForceKillTimeoutMs`, it logs `Force shutdown failed`.

A stop is complete when the service's main process has exited and its process group is empty.

A child that left the process group, for example with `setsid()`, is reached only through the cgroup. That child
gets SIGKILL without a SIGTERM first, and without a cgroup it survives the stop.

When the main process exits by itself, whatever it left behind gets the same treatment: SIGTERM to what remains
of its process group, SIGKILL after `ForceKillTimeoutMs`, and a kill of whatever is left in its cgroup. The service
shows as Stopped meanwhile. It is restarted, or started by a command, only once those processes are gone.

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
1. The service starts with the manager's environment, or with an empty one if `InheritParentEnvironment` is
   `false`. An empty environment also has no `PATH`.
2. `UseDisplay` adds `DISPLAY` and `XAUTHORITY`.
3. `EnvironmentVariables` adds its variables.

### Resource figures

- **CPU and memory:** read from the service's cgroup, so they need `CreateProcessGroup` and a manager allowed to
  create cgroups (the installed unit, run as root). Without a cgroup, both read 0.
- **GPU memory and use:** sampled through the NVIDIA driver (NVML) once a second, for the main process, its
  process group and its cgroup. Without the driver, the GUI shows "GPU unavailable".

## Monitor GUI

The monitor GUI is called Göz, "eye", and its launcher `goz`. Its mark, an eye, is the window's icon and stands
next to its name in the title row. Installed, Göz is also in the desktop's application menu.

### Starting it

```bash
goz                                  # installed launcher, manager on this machine
goz --host 10.0.0.5                  # a manager on another machine
python3 gui/goz.py --host 10.0.0.5   # straight from the source folder
```

It needs Python 3 with PyQt6 and pyzmq. It runs on Linux and Windows.

| Option | Default | Meaning |
|---|---|---|
| `--host HOST` | `127.0.0.1` | The manager's machine; the default host of the five endpoints below. |
| `--sub ENDPOINT` | `tcp://HOST:6667` | The manager's health reports. |
| `--dealer ENDPOINT` | `tcp://HOST:5557` | Commands, through the broker. |
| `--network ENDPOINT` | `tcp://HOST:6669` | The broker's status. |
| `--journal ENDPOINT` | `tcp://HOST:6670` | The journal server: the logs. |
| `--registry ENDPOINT` | `tcp://HOST:6671` | The service registry: registering, changing and removing services. |

The **Connection** button in the toolbar shows a **HOST** field, so you can switch machines without restarting
the GUI. Type the manager's machine, a name or an address, and press Enter or **Reconnect**. It goes into all five
endpoints, each keeping its port, and the GUI reconnects. A pasted endpoint or `host:port` works too; only its host
is used.

The window does not show or edit the five endpoints. A port of their own, or an endpoint on another machine, is
set with the options above when the GUI starts. When the endpoints are on different machines, HOST is empty, and
**Reconnect** with HOST empty reconnects to them as they are.

### The window

- **Title bar:** the window has no system title bar; the toolbar takes its place. Drag the toolbar to move the
  window, double-click it to maximize or restore the window, and use the buttons at its right end to minimize,
  maximize or close it. The window's edges resize it.
- **Toolbar:**
  - the feed state: **Live** while reports arrive, **Waiting for data…** before the first one, and
    **No data for N s** when they stop;
  - the manager's machine; hovering over it lists the five endpoints;
  - the **Network** pill: how many are connected to the broker, red when the broker's status stops coming;
  - the **GPU** pill;
  - **Record logs**, which reads **● Recording · N** while N services are recorded (see
    [Recording logs](#recording-logs));
  - **Connection**.
- **Sidebar:** the manager (**tepegoz**), its **network manager** (the broker), then every service
  with a dot for its state and its CPU use. An amber `*` after a name marks a service that waits for a restart
  to apply its new definition, as `*` does in `status`; hovering over the row says so. The filter box narrows
  the list. Right-click a service for **Start**, **Stop** and **Restart**, which ask for confirmation like the
  buttons on its page, and for **Edit…**, **Duplicate…** and **Remove…**; right-click anywhere in the list for
  **Add service…**, which is also the button under the list. See
  [Registering services in the GUI](#registering-services-in-the-gui).
- **A service's page:**
  - its state and pid, **restart to apply** next to the state while a new definition waits for the service's
    next start (hover over it for what that means), and **Start**, **Stop** and **Restart** (each asks for
    confirmation);
  - where the service is defined: **Defined in the main file** or **Registered**, with the file, and
    **Edit…**, **Duplicate…** and **Remove…**;
  - tiles for CPU (percent of one core), memory, GPU, VRAM, missed beats and restarts;
  - a band of its states over the last 15 minutes;
  - three tabs:
    - **Graphs**: its CPU, memory, GPU and VRAM over time. **Pop out** opens them in a larger window comparing
      services.
    - **cgroup PIDs**: the processes in its cgroup on the manager's machine, with their memory and command
      line. Double-click one to open that process's log in a window of its own. The line above the table
      says why it is empty: the service is not running, it runs without a cgroup, or the journal server does
      not answer.
    - **Journal**: the service's log.
- **The manager's page:** `systemctl status` of its unit, and its own log.
- **The network manager's page:**
  - tiles for connections, messages forwarded per second, messages not delivered and messages rejected;
  - a table of everything connected to the broker, services and other processes alike, with its address, how
    long it has been connected, its messages, their rate and when it last sent one. Services that never
    connect are not listed: a service need not use the broker;
  - the broker's events, newest first.

### Commands

**Start**, **Stop** and **Restart**, on a service's page or in its sidebar menu, behave like `--start`, `--kill`
and `--restart`. The manager answers accepted or refused, and the status bar shows the answer:
- **Each command on a new connection:** the GUI connects to the broker afresh for each command. So a command
  arrives even when another GUI is open, or when a firewall dropped the GUI's idle connection.
- **No answer:** a command the manager has not answered within 10 s is reported in the status bar; it may not
  have arrived.
- **Several GUIs:** each gets the answers to its own commands, except when two send at the same moment.

### Registering services in the GUI

The GUI registers, changes and removes services through the manager's service registry (see
[Registering services through the manager](#registering-services-through-the-manager)). It asks the registry
what is registered every 5 seconds, and again after each change it makes.

| Action | Where | For | What it does |
|---|---|---|---|
| **Add service…** | The button under the sidebar; the sidebar's menu | A new service | Opens the service dialog, empty. |
| **Edit…** | A service's page; its sidebar menu | Registered services | Opens the dialog filled from the service's definition. **Save** replaces it. |
| **Duplicate…** | A service's page; its sidebar menu | Any service | Opens the dialog filled from the service's definition, without a name. |
| **Remove…** | A service's page; its sidebar menu | Registered services | Asks for confirmation, naming the service and its file, then removes it. |

The service dialog:
- **Up front:** the name, the executable, the arguments on one line, the working folder, and **Start it now**
  (on by default; off registers the service and holds it until you start it). Edit has **Restart now to apply**
  instead, off by default.
- **Arguments:** split as a shell splits them: quote an argument that has spaces (`--title 'Front door'`).
- **Advanced:** fold it open for the work mode, the restart mode and its numbers, the health check and its
  numbers, the output and its file, the dependencies (ticked from the services the manager has), and the
  environment variables (a table of names and values). A field left empty, or at **Default**, is not written,
  so the manager's default applies; the field shows the default.
- **Paths:** typed, because the manager checks them on its own machine. **Browse…** shows only when the manager
  runs on the GUI's machine, under Linux.
- **Check:** asks the manager whether it would take the service, and shows the file it would write, or its
  problems. Nothing is written.
- **Register** (Edit: **Save**): sends the change. A refused change keeps the dialog open: the manager's answer
  shows under the fields, and each problem that names a field shows under that field. An accepted change closes
  the dialog, and the status bar says what the manager did, such as "'camera' is registered and starts".
- **Edit keeps what the form does not show:** keys the dialog has no field for, such as `UseDisplay` or
  `CreateProcessGroup`, are listed at its top and sent back as they are. A field you do not change is sent back
  as it was written, so an edit that changes nothing leaves the service unchanged.
- **Restart to apply:** a changed service that runs keeps running with its old definition and is marked
  **restart to apply** until its next start. **Restart now to apply** restarts it at once instead.
- **Removing a needed service:** when other services name it in `DependsOn`, the manager refuses, and the GUI
  lists them and offers **Remove anyway**. Those services keep running, and their next start misses it.

When the registry does not answer, every one of these actions is greyed out, and its tooltip says why: the
manager is older than the registry, or it runs on another machine without `RemoteRegistration: true`. The rest
of the GUI works as before.

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

### Recording logs

**Record logs** in the toolbar records services' logs into files on the GUI's machine:
1. Tick the services. The one on show starts ticked.
2. Pick the folder. It is `GozLogs` in your home folder until you pick another, and it is made if
   missing.
3. Press **Record**. Each service gets a file of its own, `<service>_<date>_<time>.log`, with the lines its
   journal gets from that moment on, by the manager's clock, as `journalctl -o short-iso` prints them.
4. Press **Stop**. The GUI reads each journal once more, so the files hold everything up to the stop.

While it records:
- the dialog shows each file and its line count, and can be closed; the toolbar button opens it again;
- the ticks and the folder stay as they were; to change them, stop and record again;
- a service that restarts goes on in the same file;
- if the journal server stops answering, the lines it misses are fetched once it answers again, as long as the
  journal still holds them;
- the GUI must stay open: closing it ends the recording, with the lines read up to then.

### Trying the GUI without a manager

`gui/mock_publisher.py` acts as a manager with seven fake services, on the same ports. It publishes
reports, answers commands, serves broker status and logs, and answers the service registry from services it
keeps in memory: six are the main file's, and `map_server` is registered, with `path_planner` depending on it. A
service added in the GUI joins the reports, a changed running one is marked "restart to apply", and a removed
one leaves. Its checks only stand in for the manager's: every absolute path exists, except one whose file name
starts with `missing`. Each endpoint has an option of its own (`--pub`, `--router`, `--network`, `--journal`,
`--registry`), for running it on other ports; `--registry ''` leaves the registry, and `map_server`, out.

```bash
python3 gui/mock_publisher.py
python3 gui/goz.py
```

## Ports and network access

| Port | Direction | Used by | Content |
|---|---|---|---|
| 5557 | Clients connect to the manager | Services (heartbeats), the command line, the GUI | Message broker (ZeroMQ ROUTER) |
| 6667 | Clients subscribe | GUI, `status`, `tpz_watch` | Detailed health reports, every 500 ms and at once when a service's state changes |
| 6669 | Clients subscribe | GUI | Broker status every second, and broker events (JSON) |
| 6670 | Clients connect to the manager | GUI | Journal server |
| 6671 | Clients connect to the manager | The command line, the GUI, `tpz_request` | Service registry: registers, changes and removes services. 127.0.0.1 only, unless `RemoteRegistration: true` |

The manager listens on every interface, except on 6671, which listens on 127.0.0.1 unless `RemoteRegistration:
true`. There is no authentication: anyone who can reach these ports can read the logs and send commands, and
anyone who can reach 6671 can make the manager run any program as root. Keep them behind a firewall that admits
only the machines that run the GUI.

The ports are fixed in the code, so two managers cannot run on one machine. Stop the installed service before
starting another manager by hand.

## Testing with TestServices

`manager/TestServices/` holds programs with planned behaviours and configurations that run them. Examples:
- a program that exits with a given code;
- one that crashes by a given signal;
- one that ignores SIGTERM;
- one that sends heartbeats and then stops;
- one that starts children or escapes its process group;
- one that floods its output.

`run_scenarios.py` runs the manager with each configuration and checks what it does. See
[TestServices/README.md](../manager/TestServices/README.md).

```bash
python3 manager/TestServices/scripts/run_scenarios.py --build-dir build
python3 /usr/share/tepegoz/test-setup/run_scenarios.py --prefix /usr
```

The `everything` configuration runs one program of each kind, which makes a lively manager to watch in the GUI.
