# Quick start

Tepegöz, the process manager, starts the programs listed in `tepegoz.yaml`, keeps them running and stops them.
Its monitor GUI, Göz, shows their state, resource use and logs, and starts, stops or restarts them. The GUI can run on
the manager's machine or on another one.

This page covers the everyday steps. [USAGE.md](USAGE.md) has every option, and [ARCHITECTURE.md](ARCHITECTURE.md)
explains how the parts fit together.

## 1. Install the manager

You need a Linux machine with systemd, CMake 3.20 or newer and GCC 10 or newer. Every library comes with the
source, so the build needs no network.

```bash
cmake -S Tepegoz -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j
sudo cmake --install build
```

The install puts these in place:

| What | Where |
|---|---|
| The manager | `/usr/bin/tepegoz` |
| Its configuration | `/etc/tepegoz/tepegoz.yaml` (an existing one is kept) |
| Its log files | `/var/log/tepegoz/TepegozLog/` |
| The systemd service | `tepegoz.service`, enabled so it starts at boot |
| The monitor GUI | `/usr/share/tepegoz/gui/`, started with `goz` |

The installed configuration starts no service yet. To install your own file instead, add
`-DTEPEGOZ_CONFIG_SOURCE=/path/to/tepegoz.yaml` to the first command.

## 2. List your services

Edit `/etc/tepegoz/tepegoz.yaml`. Each service needs a `ProcessName` and an absolute
`ExecutablePath`; every other key is optional.

```yaml
Tepegoz:
  Mode: Auto              # start every service when the manager starts
  LogLevel: Info

Services:
  - ProcessName: server
    ExecutablePath: /opt/app/bin/server
    Args: [--port, 9000]
    RestartMode: Always   # start it again whenever it exits

  - ProcessName: viewer
    ExecutablePath: /usr/bin/python3
    Args: [/opt/app/viewer.py]
    DependsOn: [server]   # started after server
    WorkMode: Manual      # waits until you start it
    UseDisplay: true      # gets DISPLAY and XAUTHORITY for a window
```

Check the file before you use it. `validate` reports unknown or misspelled keys, wrong values, missing programs
and dependency problems:

```bash
tepegoz validate --config /etc/tepegoz/tepegoz.yaml
```

`tepegoz generate` writes a template that documents every key, in the current folder.

## 3. Start the manager

```bash
sudo systemctl start tepegoz
systemctl status tepegoz
journalctl -u tepegoz -f             # follow its log
```

The manager reads the configuration only when it starts. After editing the file, run
`sudo systemctl restart tepegoz`.

To try a configuration without systemd, run the manager by hand. Ctrl+C stops it and every service it started.

```bash
tepegoz --config ./tepegoz.yaml
```

## 4. Control services from the command line

Run these on the manager's machine while the manager is running:

```bash
tepegoz status                   # live table of every service; Ctrl+C to leave
tepegoz status --once            # one table, then exit
tepegoz --start viewer           # start it, and the services in its DependsOn
tepegoz --kill viewer            # stop it; it stays stopped until started again
tepegoz --restart server
tepegoz --kill all               # stop every service
tepegoz --start all              # stop, rebuild and start every service, Manual ones too
```

Each command prints what it did, for example `viewer: Running, pid 4321`. It exits with 0 once that is done,
1 if it failed or the name is unknown, and 2 if it is not done within 10 s. `--no-wait` only sends the command.

## 5. Open the monitor GUI

The GUI needs Python 3 with PyQt6 and pyzmq: `python3 -m pip install PyQt6 pyzmq`.

On the manager's machine:

```bash
goz
```

From another machine, point it at the manager's machine. Copy the `Goz` folder there, or install
the project there too:

```bash
python3 Goz/goz.py --host 10.0.0.5
```

Remote use needs TCP ports 5557, 6667, 6669 and 6670 of the manager's machine to be reachable.

### Find your way around

```
┌ toolbar: Live · machine · Network pill · GPU pill · [Record logs] · [Connection] · – □ ✕ ──────┐
│ sidebar            │ the page of the selected row                                             │
│  Manager           │  process: state, Start / Stop / Restart, CPU · Memory · GPU · VRAM ·      │
│   tepegoz          │           Missed beats · Restarts, a 15-minute state band, and the tabs   │
│   network manager  │           Graphs / cgroup PIDs / Journal                                  │
│  Processes         │  manager: systemctl status and the manager's own log                     │
│   server   12 %    │  network manager: who is connected to the message broker, and its events │
│   viewer   stopped │                                                                           │
└────────────────────┴───────────────────────────────────────────────────────────────────────────┘
```

| To | Do this |
|---|---|
| See a service's state and use | Click it in the sidebar. The dot colour is its state. |
| Start, stop or restart it | Click **Start**, **Stop** or **Restart** on its page, or right-click it in the sidebar and pick one, then confirm. |
| Read its log | Open the **Journal** tab. New lines are added as they arrive. |
| Record logs into files | Click **Record logs**, tick the services, and press **Record**. **Stop** ends it. |
| See its processes | Open **cgroup PIDs**. Double-click a PID for that process's own log. |
| See its use over time | Open the **Graphs** tab. |
| Compare services over time | Click **Pop out** on the **Graphs** tab and tick the services. |
| See the manager's own log | Click **tepegoz** at the top of the sidebar. |
| Change the machine it watches | Click **Connection**, type the machine in **HOST**, and press Enter. |

## 6. When something is wrong

| You see | Look at |
|---|---|
| The toolbar shows "Waiting for data…" or "No data for …" instead of "Live" | Whether the manager runs (`systemctl status tepegoz`), and, for a remote GUI, whether a firewall blocks the ports. |
| "no answer from the journal server" in a log view or the cgroup PIDs tab | Port 6670 is blocked, or the manager is older than its journal server. |
| The cgroup PIDs tab says the service runs without a cgroup | The manager could not create cgroups: run it through its systemd unit, as root. |
| A service keeps restarting, then stays stopped | Its Journal tab, and lines such as `[name] pid 1234 exited with code 1` in the manager's log. After `MaxRestarts` restarts in a row, the manager leaves it stopped. |
| A service turns **Unhealthy** | It has `CheckHealth: true` but sends no heartbeats. See *Health checks* in [USAGE.md](USAGE.md). |
| CPU and memory read 0 | The manager has no cgroups: it is not running under the installed systemd unit, or not as root. |
| `validate` says `ExecutablePath does not exists` | The path is wrong. Paths must be absolute. |
| `validate` says `ExecutablePath access denied` | The file is not executable: `chmod +x` it. |
