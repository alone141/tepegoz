# TestServices

Small programs with planned behaviours, scenario configs that run them under the manager, and a driver that checks
what the manager does with them. Everything here is built and installed only with `ENABLE_TEST` (the default).

## Programs

Each program prints `HH:MM:SS.mmm name[pid]: message` lines and takes `--help`.

| Program | What it does | Main options |
|---|---|---|
| `pmt_steady` | Prints a line every period; on SIGTERM takes its time and exits | `--period-ms`, `--stop-delay-ms`, `--exit-code` |
| `pmt_exit` | Runs for a while, then exits with a code | `--code`, `--after-ms` |
| `pmt_crash` | Runs for a while, then dies by a signal (no core file) | `--signal SEGV\|ABRT\|KILL\|TERM\|HUP\|INT\|PIPE\|BUS\|FPE\|USR1`, `--after-ms` |
| `pmt_stubborn` | Counts and ignores SIGTERM; only SIGKILL stops it | `--child` (a child that ignores it too), `--period-ms` |
| `pmt_heartbeat` | Sends heartbeats as the service, like a real one | `--name` (its ProcessName), `--period-ms`, `--first-beat-ms`, `--beats N`, `--flap-ms` |
| `pmt_forker` | Starts children; one can escape with `setsid()`; the parent can crash | `--children`, `--escapee`, `--crash-parent-after-ms`, `--child-lifetime-ms` |
| `pmt_chatty` | Writes numbered lines to stdout and stderr | `--rate`, `--line-bytes`, `--stderr-every`, `--no-newline`, `--duration-ms` |
| `pmt_env` | Prints its arguments, working directory, ids and environment, then waits | `--show NAME`, `--all-env`, `--exit-after-ms` |
| `pmt_hog` | Keeps threads busy and holds memory | `--cpu-percent`, `--threads`, `--memory-mb` |
| `pmt_watch` | Prints the manager's health reports as text, one line per service | `--once`, `--timeout-ms`, `--endpoint` |

`pmt_heartbeat` only counts for a service with `CheckHealth: true` and a `--name` equal to its `ProcessName`.

## Scenario configs

The build writes them to `<build>/TestServices/configs/`, with the programs' paths in the build tree. The install
puts copies with the installed paths in `share/processmanager/test-setup/`.

| Config | Shows |
|---|---|
| `basic` | DependsOn start order, a Manual service, `--start`, `--kill`, `--restart` |
| `restarts` | Which exits count as failures, MaxRestarts, the backoff, the reset after a long run |
| `stopping` | A slow but graceful stop, and SIGKILL after `ForceKillTimeoutMs` for one that ignores SIGTERM |
| `health` | Heartbeats: healthy, silent then restarted, silent with `RestartMode: Never`, a late first beat |
| `process_tree` | Children in the process group, a `setsid()` escapee with and without a cgroup, orphans of a crash |
| `stdio` | `Pipe` (with long lines and no newlines), `File` with and without append, `NullDevice`, `Inherit` |
| `environment` | Args as a list and as text, environment variables, a cleared environment, the display, `Cwd` |
| `resources` | The CPU and memory figures (they need cgroups) |
| `everything` | One service of each kind, for watching in the monitor GUI |
| `registered` | A service that only a file in `services.d` defines, between two of the config's services in the start order, and `--start all` |

`validate` reports one error for `environment` on purpose: `cwd_missing` names a working directory that does not
exist, to show that the service still starts.

`registered` needs a `services.d` folder next to it, which the build does not write. The driver copies the config
into the scenario's run folder and writes `services.d/registered.yaml` next to it, together with a dot file, a
`.txt` file and a subfolder that the manager ignores. To run it by hand, make such a folder next to a copy of the
config.

Run one by hand and watch it:

```bash
build/ProcessManagerApp/src/ProcessManagerApp --config build/TestServices/configs/basic.yaml
build/TestServices/apps/pmt_watch
build/ProcessManagerApp/src/ProcessManagerApp --kill second
```

## The driver

`scripts/run_scenarios.py` runs the scenarios one after another. For each, it starts the manager, follows its
reports through `pmt_watch`, sends commands, and checks states, restart counts, timings and output. It needs only
Python 3.7 or newer, and the manager's ports (5557, 6667, 6669) free, so stop any running manager first.

```bash
python3 TestServices/scripts/run_scenarios.py --build-dir build
python3 /usr/share/processmanager/test-setup/run_scenarios.py --prefix /usr
python3 TestServices/scripts/run_scenarios.py --build-dir build health stopping
```

- **PASS / FAIL** is behaviour the scenario checks.
- **NOTE** reports behaviour worth knowing without a verdict, such as what happens to children a crashed parent
  leaves behind.
- **SKIP** marks a check that needs cgroups where the manager has none (WSL1, most containers).

The exit code is 1 when a check fails. Each scenario's manager log is kept in the run folder the driver prints.
The driver is not part of `ctest`, because it needs the manager's fixed ports.
