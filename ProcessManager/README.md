# ProcessManager Release Notes

### Build Requirements

#### Linux Dev Env.
gcc11, cmake 3.30

#### Dependencies
- libzmq with cppzmq (the `libzmq-static` target) and cxxopts: provided by the build that adds this directory.
- fkYAML 0.4.4 (MIT, header-only): vendored in `third_party/`, so an airgapped machine needs nothing more.
- GoogleTest: only for the module tests, built when `ENABLE_TEST` is on.

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
  to the identity it names; connections and undeliverable messages go to the manager's log.

### 0.1.0 
- First release
