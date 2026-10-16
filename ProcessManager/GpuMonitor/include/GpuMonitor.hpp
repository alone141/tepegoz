#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <sys/types.h>
#include <thread>
#include <unordered_map>

namespace gpu_monitor
{

constexpr int nvml_success = 0;
constexpr int nvml_error_uninitialized = 1;
constexpr int nvml_error_not_supported = 3;
constexpr int nvml_error_not_found = 6;
constexpr int nvml_error_insufficient_size = 7;
// usedGpuMemory when the driver cannot tell (NVML_VALUE_NOT_AVAILABLE).
constexpr unsigned long long nvml_value_not_available = ~0ULL;
constexpr int utilization_unknown = -1;
constexpr const char* nvml_library = "libnvidia-ml.so.1";
constexpr std::chrono::milliseconds default_period{1000};
constexpr std::chrono::milliseconds default_retry_period{10000};

struct NvmlDeviceHandle;

// NVML's nvmlDevice_t, an opaque handle.
using NvmlDevice = NvmlDeviceHandle*;

// NVML's nvmlProcessInfo_t: the layout of the _v2 and _v3 process lists.
struct NvmlProcessInfo
{
    unsigned int pid;
    unsigned long long usedGpuMemory;
    unsigned int gpuInstanceId;
    unsigned int computeInstanceId;
};

// NVML's nvmlProcessUtilizationSample_t. timeStamp is in microseconds.
struct NvmlUtilizationSample
{
    unsigned int pid;
    unsigned long long timeStamp;
    unsigned int smUtil;
    unsigned int memUtil;
    unsigned int encUtil;
    unsigned int decUtil;
};

// The NVML calls GpuSampler makes. Each returns an NVML return code.
// LoadNvml() fills them from the driver; tests fill them from a stand-in.
struct NvmlApi
{
    std::function<int()> init;
    std::function<int()> shutdown;
    std::function<int(unsigned int& count)> deviceCount;
    std::function<int(unsigned int index, NvmlDevice& device)> deviceHandle;
    std::function<int(NvmlDevice device, unsigned int& count, NvmlProcessInfo* infos)> computeProcesses;
    // Optional: when empty, graphics processes are not listed.
    std::function<int(NvmlDevice device, unsigned int& count, NvmlProcessInfo* infos)> graphicsProcesses;
    // Optional: when empty, utilization stays unknown.
    std::function<int(NvmlDevice device, NvmlUtilizationSample* samples, unsigned int& count,
                      unsigned long long lastSeen)>
        processUtilization;
};

// GPU use of one process, over every GPU it uses.
struct ProcessGpuUsage
{
    std::uint64_t memoryBytes = 0;
    // SM utilization summed over GPUs (200: two busy GPUs), or utilization_unknown.
    int utilizationPercent = utilization_unknown;
};

using GpuUsageMap = std::unordered_map<pid_t, ProcessGpuUsage>;

enum class GpuStatus
{
    Ok,
    NotSampled,
    LibraryMissing, // libnvidia-ml.so.1, or a function the sampler needs, is missing
    InitFailed,
    NoDevice,
    QueryFailed, // no GPU could be read
};

// The newest sample of a GpuMonitor, and how it went.
struct GpuSnapshot
{
    GpuStatus status = GpuStatus::NotSampled;
    // NVML's code behind InitFailed or QueryFailed.
    int nvmlError = nvml_success;
    std::chrono::steady_clock::time_point takenAt{};
    GpuUsageMap processes;
};

using NvmlLoader = std::function<GpuStatus(NvmlApi& api)>;

/**
 * @brief Load libnvidia-ml.so.1 and fill @p api with its entry points.
 * @param api Receives the NVML calls. They keep the library loaded. Left unchanged on failure.
 * @return GpuStatus::Ok, or LibraryMissing when the library or a function the sampler needs is missing.
 */
GpuStatus LoadNvml(NvmlApi& api);

class GpuSampler
{
public:
    /**
     * @brief Keep the NVML calls to sample with. Open() initializes NVML.
     * @param api NVML entry points, from LoadNvml() or a stand-in.
     */
    explicit GpuSampler(NvmlApi api);

    /**
     * @brief Shut NVML down when Open() initialized it.
     */
    ~GpuSampler();

    GpuSampler(const GpuSampler&) = delete;
    GpuSampler& operator=(const GpuSampler&) = delete;

    /**
     * @brief Take over the NVML session of @p other.
     * @param other Sampler to move from. It is left closed.
     */
    GpuSampler(GpuSampler&& other) noexcept;

    /**
     * @brief Shut this sampler's NVML session down and take over the one of @p other.
     * @param other Sampler to move from. It is left closed.
     * @return This sampler.
     */
    GpuSampler& operator=(GpuSampler&& other) noexcept;

    /**
     * @brief Initialize NVML. A sampler that is open stays open.
     * @return GpuStatus::Ok, or InitFailed with NVML's code in NvmlError().
     */
    GpuStatus Open();

    /**
     * @brief Sample the GPU use of every process on every GPU.
     * @param out Receives the use per pid on success.
     * @return GpuStatus::Ok, NoDevice without a GPU, or QueryFailed with NVML's code in NvmlError()
     *         when no GPU could be read. A GPU that cannot be read is left out of a sample that succeeds.
     */
    GpuStatus Sample(GpuUsageMap& out);

    /**
     * @brief Return NVML's code for the last failure of Open() or Sample().
     * @return An NVML return code; nvml_success before any failure.
     */
    int NvmlError() const;

private:
    // How far the utilization samples of one GPU have been read.
    struct DeviceState
    {
        bool primed = false;
        unsigned long long lastSeen = 0;
    };

    void Shutdown();
    int SampleDevice(unsigned int index, GpuUsageMap& out);
    void AddUtilization(unsigned int index, NvmlDevice device, GpuUsageMap& onDevice);

    NvmlApi api;
    bool open = false;
    int nvmlError = nvml_success;
    std::unordered_map<unsigned int, DeviceState> devices;
};

class GpuMonitor
{
public:
    /**
     * @brief Start sampling GPU use every @p period on a thread of its own.
     * @param loader Fills in the NVML calls. Tried again every @p retryPeriod while NVML is unusable.
     * @param period Time between samples.
     * @param retryPeriod Time between attempts to load and initialize NVML.
     */
    GpuMonitor(NvmlLoader loader, std::chrono::milliseconds period, std::chrono::milliseconds retryPeriod);

    /**
     * @brief Stop sampling. A thread stuck in a driver call for two seconds is left to finish alone.
     */
    ~GpuMonitor();

    GpuMonitor(const GpuMonitor&) = delete;
    GpuMonitor& operator=(const GpuMonitor&) = delete;

    /**
     * @brief Take over the sampling thread of @p other.
     * @param other Monitor to move from. It no longer samples.
     */
    GpuMonitor(GpuMonitor&& other) noexcept;

    /**
     * @brief Stop this monitor's sampling and take over the thread of @p other.
     * @param other Monitor to move from. It no longer samples.
     * @return This monitor.
     */
    GpuMonitor& operator=(GpuMonitor&& other) noexcept;

    /**
     * @brief Return the newest sample.
     * @return The newest snapshot. Its status is NotSampled before the first sample.
     */
    GpuSnapshot Latest() const;

private:
    struct Shared;

    static void Run(std::shared_ptr<Shared> shared, NvmlLoader loader, std::chrono::milliseconds period,
                    std::chrono::milliseconds retryPeriod);
    void Stop();

    std::shared_ptr<Shared> shared;
    std::thread worker;
};

} // namespace gpu_monitor
