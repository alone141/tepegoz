# ProcessManager Release Notes

### Build Requirements

#### Linux Dev Env.
gcc11, cmake 3.30

### Unreleased
- The manager samples each service's GPU use through NVML, loaded at run time, and sends it in the health
  reports, which grow from 128 to 144 bytes. The monitor GUI reads it from there instead of running nvidia-smi.

### 0.1.0 
- First release
