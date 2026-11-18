#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctype.h>
#include <sys/types.h>
namespace tepegoz
{

enum class RuntimeState : uint8_t
{
    Unknown,
    Starting,
    Running,
    Stopped,
    Unhealthy
};
[[maybe_unused]]
static const char* RuntimeStateToString(RuntimeState e)
{
    switch (e)
    {
    case RuntimeState::Unknown:
        return "Unknown";
        break;
    case RuntimeState::Starting:
        return "Starting";
        break;
    case RuntimeState::Running:
        return "Running";
        break;
    case RuntimeState::Stopped:
        return "Stopped";
        break;
    case RuntimeState::Unhealthy:
        return "Unhealthy";
        break;
    default:
        return "Unknown";
        break;
    }
}
struct RuntimeHealth
{
    RuntimeState state = RuntimeState::Unknown;
    // In the health report: 1 while a reloaded definition waits for the service's next start ("restart to apply").
    // It takes a byte that was padding, so the report keeps its size and every other field its offset.
    uint8_t restartToApply = 0;

    std::chrono::high_resolution_clock::time_point startTime;
    std::chrono::high_resolution_clock::time_point lastSeen;

    int missedBeats = 0;
    int restartCount = 0;


};
// Wire values: the GUI reads them.
enum class GpuUsageState : uint8_t
{
    NotSampled, // the manager has not sampled the GPUs yet
    Sampled,
    NoDriver,   // libnvidia-ml.so.1 could not be loaded
    NoDevice,   // NVML finds no GPU
    Failed,     // NVML failed; the manager's log has its error code
    Stalled     // no new sample for a while: a driver call is not returning
};
struct GpuUsage
{
    uint64_t memoryUsageInBytes = 0; // every process of the service, on every GPU; 0 unless Sampled (the GUI checks it)
    int32_t utilizationPercent = -1; // SM use summed over its processes and GPUs (200: two busy GPUs); -1 unknown
    GpuUsageState state = GpuUsageState::NotSampled; // the other fields count only when Sampled
    uint8_t reserved[3] = {}; // sent as raw bytes: no padding left uninitialized
};
struct DetailedHealthReport
{
    char processName[64];
    pid_t pid;
    uint64_t memoryUsageInBytes;
    uint64_t cpuUsageInUsec;

    RuntimeHealth runtimeHealth;

    std::chrono::high_resolution_clock::time_point snapshotTime;
    GpuUsage gpuUsage;
};
static_assert(sizeof(DetailedHealthReport) == 144, "the GUI parses 144-byte health reports");
// The offsets the GUI (health_structs.py) reads the fields at.
static_assert(offsetof(DetailedHealthReport, processName) == 0, "processName moved");
static_assert(offsetof(DetailedHealthReport, pid) == 64, "pid moved");
static_assert(offsetof(DetailedHealthReport, memoryUsageInBytes) == 72, "memoryUsageInBytes moved");
static_assert(offsetof(DetailedHealthReport, cpuUsageInUsec) == 80, "cpuUsageInUsec moved");
static_assert(offsetof(DetailedHealthReport, runtimeHealth.state) == 88, "runtimeHealth.state moved");
static_assert(offsetof(DetailedHealthReport, runtimeHealth.restartToApply) == 89, "restartToApply is not the padding byte after state");
static_assert(offsetof(DetailedHealthReport, runtimeHealth.startTime) == 96, "runtimeHealth.startTime moved");
static_assert(offsetof(DetailedHealthReport, runtimeHealth.lastSeen) == 104, "runtimeHealth.lastSeen moved");
static_assert(offsetof(DetailedHealthReport, runtimeHealth.missedBeats) == 112, "runtimeHealth.missedBeats moved");
static_assert(offsetof(DetailedHealthReport, runtimeHealth.restartCount) == 116, "runtimeHealth.restartCount moved");
static_assert(offsetof(DetailedHealthReport, snapshotTime) == 120, "snapshotTime moved");
static_assert(offsetof(DetailedHealthReport, gpuUsage.memoryUsageInBytes) == 128, "gpuUsage.memoryUsageInBytes moved");
static_assert(offsetof(DetailedHealthReport, gpuUsage.utilizationPercent) == 136, "gpuUsage.utilizationPercent moved");
static_assert(offsetof(DetailedHealthReport, gpuUsage.state) == 140, "gpuUsage.state moved");

} // namespace tepegoz
