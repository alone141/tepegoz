# <img src="gui/goz.svg" alt="" width="40" height="40"> Tepegöz

Tepegöz starts the programs of a machine, keeps them running and stops them. Its monitor GUI, Göz, shows their
state, resource use and logs, and starts and stops them, from the same machine or another one.

- [Quick start](docs/QUICKSTART.md): install, list your services, start the manager, use the GUI.
- [Usage guide](docs/USAGE.md): every command, configuration key and GUI feature.
- [Architecture](docs/ARCHITECTURE.md): threads, messaging and wire formats, supervision, the GUI's internals.

| Folder | Contents |
|---|---|
| `manager/` | The manager (C++20, CMake). Its `README.md` has the build requirements and release notes. |
| `gui/` | Göz, the monitor GUI (Python 3.8 or newer, PyQt6, pyzmq). |
| `docs/` | The documents above. |
