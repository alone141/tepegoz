#include "GpuMonitor.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <dlfcn.h>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gpu_monitor
{

namespace
{

constexpr std::size_t initial_list_size = 64;
constexpr std::size_t list_headroom = 16;
constexpr int max_list_attempts = 4;
constexpr unsigned int max_utilization = 100;
constexpr std::chrono::milliseconds stop_timeout{2000};

// NVML's nvmlProcessInfo_v1_t, for drivers without the _v2 process lists.
struct NvmlProcessInfoV1
{
    unsigned int pid;
    unsigned long long usedGpuMemory;
};

using InitFunction = int (*)();
using CountFunction = int (*)(unsigned int*);
using HandleFunction = int (*)(unsigned int, NvmlDevice*);
using ProcessesFunction = int (*)(NvmlDevice, unsigned int*, NvmlProcessInfo*);
using ProcessesV1Function = int (*)(NvmlDevice, unsigned int*, NvmlProcessInfoV1*);
using UtilizationFunction = int (*)(NvmlDevice, NvmlUtilizationSample*, unsigned int*, unsigned long long);
using ProcessQuery = std::function<int(NvmlDevice device, unsigned int& count, NvmlProcessInfo* infos)>;
using SampleMap = std::unordered_map<pid_t, NvmlUtilizationSample>;

void CloseLibrary(void* library)
{
    ::dlclose(library);
}

template <typename Function>
Function FindFunction(void* library, const std::string& name)
{
    return reinterpret_cast<Function>(::dlsym(library, name.c_str()));
}

// The newest list the driver has: _v3, then _v2 (the same layout), then the original v1.
ProcessQuery FindProcessQuery(const std::shared_ptr<void>& library, const std::string& name)
{
    for (const char* suffix : {"_v3", "_v2"})
    {
        const ProcessesFunction query = FindFunction<ProcessesFunction>(library.get(), name + suffix);
        if (query != nullptr)
        {
            return [library, query](NvmlDevice device, unsigned int& count, NvmlProcessInfo* infos)
            {
                return query(device, &count, infos);
            };
        }
    }

    const ProcessesV1Function queryV1 = FindFunction<ProcessesV1Function>(library.get(), name);
    if (queryV1 == nullptr)
    {
        return ProcessQuery{};
    }

    return [library, queryV1](NvmlDevice device, unsigned int& count, NvmlProcessInfo* infos)
    {
        std::vector<NvmlProcessInfoV1> listed(infos == nullptr ? 0 : count);
        const int code = queryV1(device, &count, listed.empty() ? nullptr : listed.data());
        if (code == nvml_success)
        {
            for (std::size_t i = 0; i < count && i < listed.size(); ++i)
            {
                infos[i] = NvmlProcessInfo{listed[i].pid, listed[i].usedGpuMemory, ~0U, ~0U};
            }
        }

        return code;
    };
}

// NVML's list calls fail with INSUFFICIENT_SIZE, and the size they need, when the buffer is too small.
template <typename Item, typename Query>
int ReadList(const Query& query, std::vector<Item>& items)
{
    std::size_t size = initial_list_size;
    for (int attempt = 0; attempt < max_list_attempts; ++attempt)
    {
        items.assign(size, Item{});
        unsigned int count = static_cast<unsigned int>(items.size());
        const int code = query(items.data(), count);
        if (code == nvml_success)
        {
            items.resize(std::min(static_cast<std::size_t>(count), items.size()));
            return code;
        }

        if (code != nvml_error_insufficient_size)
        {
            items.clear();
            return code;
        }

        // More processes can start before the next call: leave room for them.
        size = static_cast<std::size_t>(count) + list_headroom;
    }

    items.clear();
    return nvml_error_insufficient_size;
}

// A process on both the compute and the graphics list of a GPU is counted once.
void AddProcess(const NvmlProcessInfo& process, GpuUsageMap& onDevice)
{
    if (process.pid == 0)
    {
        return;
    }

    const std::uint64_t memory = process.usedGpuMemory == nvml_value_not_available ? 0 : process.usedGpuMemory;
    ProcessGpuUsage& usage = onDevice[static_cast<pid_t>(process.pid)];
    usage.memoryBytes = std::max(usage.memoryBytes, memory);
}

void AddUsage(const ProcessGpuUsage& usage, ProcessGpuUsage& sum)
{
    sum.memoryBytes += usage.memoryBytes;
    if (usage.utilizationPercent != utilization_unknown)
    {
        sum.utilizationPercent = std::max(sum.utilizationPercent, 0) + usage.utilizationPercent;
    }
}

GpuStatus Connect(const NvmlLoader& loader, std::unique_ptr<GpuSampler>& out, int& nvmlError)
{
    NvmlApi api{};
    const GpuStatus loaded = loader(api);
    if (loaded != GpuStatus::Ok)
    {
        return loaded;
    }

    std::unique_ptr<GpuSampler> sampler = std::make_unique<GpuSampler>(std::move(api));
    const GpuStatus opened = sampler->Open();
    nvmlError = sampler->NvmlError();
    if (opened == GpuStatus::Ok)
    {
        out = std::move(sampler);
    }

    return opened;
}

} // namespace

GpuStatus LoadNvml(NvmlApi& api)
{
    void* handle = ::dlopen(nvml_library, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr)
    {
        return GpuStatus::LibraryMissing;
    }

    const std::shared_ptr<void> library{handle, CloseLibrary};
    const InitFunction init = FindFunction<InitFunction>(handle, "nvmlInit_v2");
    const InitFunction shutdown = FindFunction<InitFunction>(handle, "nvmlShutdown");
    const CountFunction deviceCount = FindFunction<CountFunction>(handle, "nvmlDeviceGetCount_v2");
    const HandleFunction deviceHandle = FindFunction<HandleFunction>(handle, "nvmlDeviceGetHandleByIndex_v2");
    const UtilizationFunction utilization =
        FindFunction<UtilizationFunction>(handle, "nvmlDeviceGetProcessUtilization");
    ProcessQuery computeProcesses = FindProcessQuery(library, "nvmlDeviceGetComputeRunningProcesses");
    if (init == nullptr || shutdown == nullptr || deviceCount == nullptr || deviceHandle == nullptr ||
        !computeProcesses)
    {
        return GpuStatus::LibraryMissing;
    }

    NvmlApi loaded{};
    loaded.init = [library, init]()
    {
        return init();
    };
    loaded.shutdown = [library, shutdown]()
    {
        return shutdown();
    };
    loaded.deviceCount = [library, deviceCount](unsigned int& count)
    {
        return deviceCount(&count);
    };
    loaded.deviceHandle = [library, deviceHandle](unsigned int index, NvmlDevice& device)
    {
        return deviceHandle(index, &device);
    };
    loaded.computeProcesses = std::move(computeProcesses);
    loaded.graphicsProcesses = FindProcessQuery(library, "nvmlDeviceGetGraphicsRunningProcesses");
    if (utilization != nullptr)
    {
        loaded.processUtilization = [library, utilization](NvmlDevice device, NvmlUtilizationSample* samples,
                                                           unsigned int& count, unsigned long long lastSeen)
        {
            return utilization(device, samples, &count, lastSeen);
        };
    }

    api = std::move(loaded);
    return GpuStatus::Ok;
}

GpuSampler::GpuSampler(NvmlApi api)
    : api{std::move(api)}
{
}

GpuSampler::~GpuSampler()
{
    Shutdown();
}

GpuSampler::GpuSampler(GpuSampler&& other) noexcept
    : api{std::move(other.api)},
      open{other.open},
      nvmlError{other.nvmlError},
      devices{std::move(other.devices)}
{
    other.open = false;
}

GpuSampler& GpuSampler::operator=(GpuSampler&& other) noexcept
{
    if (this != &other)
    {
        Shutdown();
        api = std::move(other.api);
        open = other.open;
        nvmlError = other.nvmlError;
        devices = std::move(other.devices);
        other.open = false;
    }

    return *this;
}

GpuStatus GpuSampler::Open()
{
    if (open)
    {
        return GpuStatus::Ok;
    }

    const int code = api.init();
    if (code != nvml_success)
    {
        nvmlError = code;
        return GpuStatus::InitFailed;
    }

    open = true;
    devices.clear();
    return GpuStatus::Ok;
}

GpuStatus GpuSampler::Sample(GpuUsageMap& out)
{
    if (!open)
    {
        nvmlError = nvml_error_uninitialized;
        return GpuStatus::QueryFailed;
    }

    unsigned int count = 0;
    const int countCode = api.deviceCount(count);
    if (countCode != nvml_success)
    {
        nvmlError = countCode;
        return GpuStatus::QueryFailed;
    }

    if (count == 0)
    {
        return GpuStatus::NoDevice;
    }

    GpuUsageMap total;
    bool anyRead = false;
    for (unsigned int index = 0; index < count; ++index)
    {
        GpuUsageMap onDevice;
        const int code = SampleDevice(index, onDevice);
        if (code != nvml_success)
        {
            nvmlError = code;
            continue;
        }

        anyRead = true;
        for (const GpuUsageMap::value_type& entry : onDevice)
        {
            AddUsage(entry.second, total[entry.first]);
        }
    }

    if (!anyRead)
    {
        return GpuStatus::QueryFailed;
    }

    out = std::move(total);
    return GpuStatus::Ok;
}

int GpuSampler::NvmlError() const
{
    return nvmlError;
}

void GpuSampler::Shutdown()
{
    if (open && api.shutdown)
    {
        api.shutdown();
    }

    open = false;
}

int GpuSampler::SampleDevice(unsigned int index, GpuUsageMap& out)
{
    NvmlDevice device = nullptr;
    const int handleCode = api.deviceHandle(index, device);
    if (handleCode != nvml_success)
    {
        return handleCode;
    }

    std::vector<NvmlProcessInfo> processes;
    const int computeCode = ReadList(
        [this, device](NvmlProcessInfo* infos, unsigned int& count)
        {
            return api.computeProcesses(device, count, infos);
        },
        processes);
    if (computeCode != nvml_success && computeCode != nvml_error_not_supported)
    {
        return computeCode;
    }

    for (const NvmlProcessInfo& process : processes)
    {
        AddProcess(process, out);
    }

    // The graphics list is optional: when it cannot be read it comes back empty, and the
    // GPU still counts with its compute list.
    if (api.graphicsProcesses)
    {
        ReadList(
            [this, device](NvmlProcessInfo* infos, unsigned int& count)
            {
                return api.graphicsProcesses(device, count, infos);
            },
            processes);
        for (const NvmlProcessInfo& process : processes)
        {
            AddProcess(process, out);
        }
    }

    AddUtilization(index, device, out);
    return nvml_success;
}

void GpuSampler::AddUtilization(unsigned int index, NvmlDevice device, GpuUsageMap& onDevice)
{
    if (!api.processUtilization)
    {
        return;
    }

    DeviceState& state = devices[index];
    const unsigned long long lastSeen = state.lastSeen;
    std::vector<NvmlUtilizationSample> samples;
    const int code = ReadList(
        [this, device, lastSeen](NvmlUtilizationSample* buffer, unsigned int& count)
        {
            return api.processUtilization(device, buffer, count, lastSeen);
        },
        samples);
    // NOT_FOUND: no process used the GPU since lastSeen. Any other failure, NOT_SUPPORTED
    // among them, leaves the utilization unknown.
    if (code != nvml_success && code != nvml_error_not_found)
    {
        return;
    }

    SampleMap newest;
    for (const NvmlUtilizationSample& sample : samples)
    {
        // Drivers have returned samples above 100 % and samples older than the ones asked for.
        if (sample.pid == 0 || sample.smUtil > max_utilization || sample.timeStamp <= lastSeen)
        {
            continue;
        }

        state.lastSeen = std::max(state.lastSeen, sample.timeStamp);
        NvmlUtilizationSample& kept = newest[static_cast<pid_t>(sample.pid)];
        if (sample.timeStamp >= kept.timeStamp)
        {
            kept = sample;
        }
    }

    // The first read only finds where the samples end: the ones before it can be long out of date.
    if (!state.primed)
    {
        state.primed = true;
        return;
    }

    for (GpuUsageMap::value_type& entry : onDevice)
    {
        entry.second.utilizationPercent = 0; // on the GPU, but no sample since the last read: idle
    }

    for (const SampleMap::value_type& entry : newest)
    {
        onDevice[entry.first].utilizationPercent = static_cast<int>(entry.second.smUtil);
    }
}

struct GpuMonitor::Shared
{
    std::mutex mutex;
    std::condition_variable changed;
    bool stopping = false;
    bool finished = false;
    GpuSnapshot latest;
};

GpuMonitor::GpuMonitor(NvmlLoader loader, std::chrono::milliseconds period, std::chrono::milliseconds retryPeriod)
    : shared{std::make_shared<Shared>()},
      worker{Run, shared, std::move(loader), period, retryPeriod}
{
}

GpuMonitor::~GpuMonitor()
{
    Stop();
}

GpuMonitor::GpuMonitor(GpuMonitor&& other) noexcept
    : shared{std::move(other.shared)},
      worker{std::move(other.worker)}
{
}

GpuMonitor& GpuMonitor::operator=(GpuMonitor&& other) noexcept
{
    if (this != &other)
    {
        Stop();
        shared = std::move(other.shared);
        worker = std::move(other.worker);
    }

    return *this;
}

GpuSnapshot GpuMonitor::Latest() const
{
    if (shared == nullptr)
    {
        return GpuSnapshot{};
    }

    const std::lock_guard<std::mutex> lock{shared->mutex};
    return shared->latest;
}

void GpuMonitor::Run(std::shared_ptr<Shared> shared, NvmlLoader loader, std::chrono::milliseconds period,
                     std::chrono::milliseconds retryPeriod)
{
    std::unique_ptr<GpuSampler> sampler;
    GpuStatus status = GpuStatus::NotSampled;
    int nvmlError = nvml_success;
    std::chrono::steady_clock::time_point retryAt{};

    const auto stopRequested = [&shared]()
    {
        return shared->stopping;
    };

    std::unique_lock<std::mutex> lock{shared->mutex};
    while (!shared->stopping)
    {
        lock.unlock();
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (sampler == nullptr && now >= retryAt)
        {
            nvmlError = nvml_success;
            status = Connect(loader, sampler, nvmlError);
            if (sampler == nullptr)
            {
                retryAt = now + retryPeriod;
            }
        }

        GpuSnapshot snapshot{};
        if (sampler != nullptr)
        {
            status = sampler->Sample(snapshot.processes);
            nvmlError = sampler->NvmlError();
            if (status == GpuStatus::QueryFailed)
            {
                sampler.reset(); // initialized again after the retry period
                retryAt = now + retryPeriod;
            }
        }

        snapshot.status = status;
        snapshot.nvmlError = nvmlError;
        snapshot.takenAt = std::chrono::steady_clock::now();
        lock.lock();
        shared->latest = std::move(snapshot);
        shared->changed.wait_for(lock, period, stopRequested);
    }

    lock.unlock();
    sampler.reset();
    lock.lock();
    shared->finished = true;
    shared->changed.notify_all();
}

void GpuMonitor::Stop()
{
    if (shared == nullptr)
    {
        return;
    }

    const Shared* state = shared.get();
    const auto finishedRunning = [state]()
    {
        return state->finished;
    };

    std::unique_lock<std::mutex> lock{shared->mutex};
    shared->stopping = true;
    shared->changed.notify_all();
    const bool finished = shared->changed.wait_for(lock, stop_timeout, finishedRunning);
    lock.unlock();
    if (finished)
    {
        worker.join();
    }
    else
    {
        worker.detach(); // stuck in a driver call; the thread keeps its own reference to the shared state
    }

    shared.reset();
}

} // namespace gpu_monitor
