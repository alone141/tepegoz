#include <gtest/gtest.h>

#include "GpuMonitor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace
{

constexpr std::uint64_t mib = 1024 * 1024;
constexpr std::chrono::milliseconds fast_period{5};
constexpr std::chrono::milliseconds slow_period{10000};
constexpr std::chrono::milliseconds wait_limit{3000};
constexpr int nvml_error_driver_not_loaded = 9;
constexpr int nvml_error_gpu_is_lost = 15;

// One GPU of the stand-in driver.
struct FakeGpu
{
    std::vector<gpu_monitor::NvmlProcessInfo> compute;
    std::vector<gpu_monitor::NvmlProcessInfo> graphics;
    std::vector<gpu_monitor::NvmlUtilizationSample> samples;
    int handleCode = gpu_monitor::nvml_success;
    int utilizationCode = gpu_monitor::nvml_success;
};

// The stand-in driver: its GPUs, and what the sampler asked of it.
struct FakeNvml
{
    std::vector<FakeGpu> gpus;
    int initCode = gpu_monitor::nvml_success;
    std::atomic<int> shutdowns{0};
    std::vector<unsigned long long> lastSeenAsked;
};

// Holds a driver call until it is opened, like a GPU that stopped answering.
struct Gate
{
    std::mutex mutex;
    std::condition_variable opened;
    bool open = false;
    std::atomic<bool> entered{false};
    std::atomic<bool> shutDown{false};
};

gpu_monitor::NvmlDevice HandleOf(unsigned int index)
{
    return reinterpret_cast<gpu_monitor::NvmlDevice>(static_cast<std::uintptr_t>(index) + 1);
}

unsigned int IndexOf(gpu_monitor::NvmlDevice device)
{
    return static_cast<unsigned int>(reinterpret_cast<std::uintptr_t>(device) - 1);
}

// Fills @p buffer as NVML does: a buffer that is too small gets the size it needs instead.
template <typename List, typename Item>
int FillList(const List& items, Item* buffer, unsigned int& count)
{
    const unsigned int needed = static_cast<unsigned int>(items.size());
    if (needed > 0 && (buffer == nullptr || count < needed))
    {
        count = needed;
        return gpu_monitor::nvml_error_insufficient_size;
    }

    std::copy(items.begin(), items.end(), buffer);
    count = needed;
    return gpu_monitor::nvml_success;
}

gpu_monitor::NvmlApi MakeApi(const std::shared_ptr<FakeNvml>& fake)
{
    gpu_monitor::NvmlApi api{};
    api.init = [fake]()
    {
        return fake->initCode;
    };
    api.shutdown = [fake]()
    {
        ++fake->shutdowns;
        return gpu_monitor::nvml_success;
    };
    api.deviceCount = [fake](unsigned int& count)
    {
        count = static_cast<unsigned int>(fake->gpus.size());
        return gpu_monitor::nvml_success;
    };
    api.deviceHandle = [fake](unsigned int index, gpu_monitor::NvmlDevice& device)
    {
        device = HandleOf(index);
        return fake->gpus.at(index).handleCode;
    };
    api.computeProcesses = [fake](gpu_monitor::NvmlDevice device, unsigned int& count,
                                  gpu_monitor::NvmlProcessInfo* infos)
    {
        return FillList(fake->gpus.at(IndexOf(device)).compute, infos, count);
    };
    api.graphicsProcesses = [fake](gpu_monitor::NvmlDevice device, unsigned int& count,
                                   gpu_monitor::NvmlProcessInfo* infos)
    {
        return FillList(fake->gpus.at(IndexOf(device)).graphics, infos, count);
    };
    api.processUtilization = [fake](gpu_monitor::NvmlDevice device, gpu_monitor::NvmlUtilizationSample* samples,
                                    unsigned int& count, unsigned long long lastSeen)
    {
        fake->lastSeenAsked.push_back(lastSeen);
        const FakeGpu& gpu = fake->gpus.at(IndexOf(device));
        if (gpu.utilizationCode != gpu_monitor::nvml_success)
        {
            return gpu.utilizationCode;
        }

        std::vector<gpu_monitor::NvmlUtilizationSample> newer;
        for (const gpu_monitor::NvmlUtilizationSample& sample : gpu.samples)
        {
            if (sample.timeStamp > lastSeen)
            {
                newer.push_back(sample);
            }
        }

        if (newer.empty())
        {
            return gpu_monitor::nvml_error_not_found;
        }

        return FillList(newer, samples, count);
    };
    return api;
}

gpu_monitor::NvmlLoader LoaderOf(const gpu_monitor::NvmlApi& api)
{
    return [api](gpu_monitor::NvmlApi& out)
    {
        out = api;
        return gpu_monitor::GpuStatus::Ok;
    };
}

gpu_monitor::NvmlProcessInfo ListedProcess(unsigned int pid, std::uint64_t memory)
{
    return gpu_monitor::NvmlProcessInfo{pid, memory, ~0U, ~0U};
}

gpu_monitor::NvmlUtilizationSample UtilizationSample(unsigned int pid, unsigned long long timeStamp,
                                                     unsigned int smUtil)
{
    return gpu_monitor::NvmlUtilizationSample{pid, timeStamp, smUtil, 0, 0, 0};
}

// Opens a sampler on @p fake and takes one sample, which primes the utilization reads.
gpu_monitor::GpuSampler OpenPrimed(const std::shared_ptr<FakeNvml>& fake)
{
    gpu_monitor::GpuSampler sampler{MakeApi(fake)};
    gpu_monitor::GpuUsageMap first{};
    EXPECT_EQ(sampler.Open(), gpu_monitor::GpuStatus::Ok);
    EXPECT_EQ(sampler.Sample(first), gpu_monitor::GpuStatus::Ok);
    return sampler;
}

template <typename Predicate>
bool WaitFor(const gpu_monitor::GpuMonitor& monitor, Predicate done)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + wait_limit;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (done(monitor.Latest()))
        {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    return false;
}

bool WaitUntil(const std::atomic<bool>& flag)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + wait_limit;
    while (!flag && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    return flag;
}

// Blocks the calling thread until OpenGate().
void PassGate(Gate& gate)
{
    gate.entered = true;
    std::unique_lock<std::mutex> lock{gate.mutex};
    gate.opened.wait(lock,
                     [&gate]()
                     {
                         return gate.open;
                     });
}

void OpenGate(Gate& gate)
{
    {
        const std::lock_guard<std::mutex> lock{gate.mutex};
        gate.open = true;
    }

    gate.opened.notify_all();
}

bool IsOk(const gpu_monitor::GpuSnapshot& snapshot)
{
    return snapshot.status == gpu_monitor::GpuStatus::Ok;
}

} // namespace

TEST(GpuSamplerTest, SumsMemoryOverGpus)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(2);
    fake->gpus[0].compute = {ListedProcess(100, 512 * mib)};
    fake->gpus[1].compute = {ListedProcess(100, 256 * mib), ListedProcess(200, 64 * mib)};
    gpu_monitor::GpuSampler sampler{MakeApi(fake)};
    gpu_monitor::GpuUsageMap usage{};
    ASSERT_EQ(sampler.Open(), gpu_monitor::GpuStatus::Ok);
    ASSERT_EQ(sampler.Sample(usage), gpu_monitor::GpuStatus::Ok);
    EXPECT_EQ(usage.at(100).memoryBytes, 768 * mib);
    EXPECT_EQ(usage.at(200).memoryBytes, 64 * mib);
}

TEST(GpuSamplerTest, CountsProcessOnBothListsOnce)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(1);
    fake->gpus[0].compute = {ListedProcess(100, 300 * mib)};
    fake->gpus[0].graphics = {ListedProcess(100, 300 * mib), ListedProcess(300, 40 * mib)};
    gpu_monitor::GpuSampler sampler{MakeApi(fake)};
    gpu_monitor::GpuUsageMap usage{};
    ASSERT_EQ(sampler.Open(), gpu_monitor::GpuStatus::Ok);
    ASSERT_EQ(sampler.Sample(usage), gpu_monitor::GpuStatus::Ok);
    EXPECT_EQ(usage.at(100).memoryBytes, 300 * mib);
    EXPECT_EQ(usage.at(300).memoryBytes, 40 * mib);
}

TEST(GpuSamplerTest, UnavailableMemoryCountsAsZero)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(1);
    fake->gpus[0].compute = {ListedProcess(100, gpu_monitor::nvml_value_not_available)};
    gpu_monitor::GpuSampler sampler{MakeApi(fake)};
    gpu_monitor::GpuUsageMap usage{};
    ASSERT_EQ(sampler.Open(), gpu_monitor::GpuStatus::Ok);
    ASSERT_EQ(sampler.Sample(usage), gpu_monitor::GpuStatus::Ok);
    ASSERT_EQ(usage.count(100), 1U);
    EXPECT_EQ(usage.at(100).memoryBytes, 0U);
}

TEST(GpuSamplerTest, GrowsBufferWhenDriverNeedsMore)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(1);
    for (unsigned int pid = 1; pid <= 150; ++pid)
    {
        fake->gpus[0].compute.push_back(ListedProcess(pid, mib));
    }

    gpu_monitor::GpuSampler sampler{MakeApi(fake)};
    gpu_monitor::GpuUsageMap usage{};
    ASSERT_EQ(sampler.Open(), gpu_monitor::GpuStatus::Ok);
    ASSERT_EQ(sampler.Sample(usage), gpu_monitor::GpuStatus::Ok);
    EXPECT_EQ(usage.size(), 150U);
}

TEST(GpuSamplerTest, FirstSampleLeavesUtilizationUnknown)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(1);
    fake->gpus[0].compute = {ListedProcess(100, mib)};
    fake->gpus[0].samples = {UtilizationSample(100, 10, 90)};
    gpu_monitor::GpuSampler sampler{MakeApi(fake)};
    gpu_monitor::GpuUsageMap usage{};
    ASSERT_EQ(sampler.Open(), gpu_monitor::GpuStatus::Ok);
    ASSERT_EQ(sampler.Sample(usage), gpu_monitor::GpuStatus::Ok);
    EXPECT_EQ(usage.at(100).utilizationPercent, gpu_monitor::utilization_unknown);
}

TEST(GpuSamplerTest, TakesNewestUtilizationSample)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(1);
    fake->gpus[0].compute = {ListedProcess(100, mib)};
    fake->gpus[0].samples = {UtilizationSample(100, 10, 5)};
    gpu_monitor::GpuSampler sampler = OpenPrimed(fake);
    fake->gpus[0].samples.push_back(UtilizationSample(100, 30, 50));
    fake->gpus[0].samples.push_back(UtilizationSample(100, 20, 30));
    gpu_monitor::GpuUsageMap usage{};
    ASSERT_EQ(sampler.Sample(usage), gpu_monitor::GpuStatus::Ok);
    EXPECT_EQ(usage.at(100).utilizationPercent, 50);
}

TEST(GpuSamplerTest, AsksOnlyForSamplesNewerThanLastRead)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(1);
    fake->gpus[0].compute = {ListedProcess(100, mib)};
    fake->gpus[0].samples = {UtilizationSample(100, 10, 5), UtilizationSample(100, 40, 7)};
    gpu_monitor::GpuSampler sampler = OpenPrimed(fake);
    gpu_monitor::GpuUsageMap usage{};
    ASSERT_EQ(sampler.Sample(usage), gpu_monitor::GpuStatus::Ok);
    ASSERT_FALSE(fake->lastSeenAsked.empty());
    EXPECT_EQ(fake->lastSeenAsked.back(), 40U);
}

TEST(GpuSamplerTest, ProcessWithoutNewSampleIsIdle)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(1);
    fake->gpus[0].compute = {ListedProcess(100, mib)};
    fake->gpus[0].samples = {UtilizationSample(100, 10, 80)};
    gpu_monitor::GpuSampler sampler = OpenPrimed(fake);
    gpu_monitor::GpuUsageMap usage{};
    ASSERT_EQ(sampler.Sample(usage), gpu_monitor::GpuStatus::Ok);
    EXPECT_EQ(usage.at(100).utilizationPercent, 0);
}

TEST(GpuSamplerTest, IgnoresSamplesAboveHundredPercent)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(1);
    fake->gpus[0].compute = {ListedProcess(100, mib)};
    gpu_monitor::GpuSampler sampler = OpenPrimed(fake);
    fake->gpus[0].samples = {UtilizationSample(100, 10, 250)};
    gpu_monitor::GpuUsageMap usage{};
    ASSERT_EQ(sampler.Sample(usage), gpu_monitor::GpuStatus::Ok);
    EXPECT_EQ(usage.at(100).utilizationPercent, 0);
}

TEST(GpuSamplerTest, SumsUtilizationOverGpus)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(2);
    fake->gpus[0].compute = {ListedProcess(100, mib)};
    fake->gpus[1].compute = {ListedProcess(100, mib)};
    gpu_monitor::GpuSampler sampler = OpenPrimed(fake);
    fake->gpus[0].samples = {UtilizationSample(100, 10, 40)};
    fake->gpus[1].samples = {UtilizationSample(100, 10, 30)};
    gpu_monitor::GpuUsageMap usage{};
    ASSERT_EQ(sampler.Sample(usage), gpu_monitor::GpuStatus::Ok);
    EXPECT_EQ(usage.at(100).utilizationPercent, 70);
}

TEST(GpuSamplerTest, UnsupportedUtilizationStaysUnknown)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(1);
    fake->gpus[0].compute = {ListedProcess(100, 128 * mib)};
    fake->gpus[0].utilizationCode = gpu_monitor::nvml_error_not_supported;
    gpu_monitor::GpuSampler sampler = OpenPrimed(fake);
    gpu_monitor::GpuUsageMap usage{};
    ASSERT_EQ(sampler.Sample(usage), gpu_monitor::GpuStatus::Ok);
    EXPECT_EQ(usage.at(100).utilizationPercent, gpu_monitor::utilization_unknown);
    EXPECT_EQ(usage.at(100).memoryBytes, 128 * mib);
}

TEST(GpuSamplerTest, NoGpuReturnsNoDevice)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    gpu_monitor::GpuSampler sampler{MakeApi(fake)};
    gpu_monitor::GpuUsageMap usage{};
    ASSERT_EQ(sampler.Open(), gpu_monitor::GpuStatus::Ok);
    EXPECT_EQ(sampler.Sample(usage), gpu_monitor::GpuStatus::NoDevice);
}

TEST(GpuSamplerTest, InitFailureReturnsInitFailed)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->initCode = nvml_error_driver_not_loaded;
    gpu_monitor::GpuSampler sampler{MakeApi(fake)};
    EXPECT_EQ(sampler.Open(), gpu_monitor::GpuStatus::InitFailed);
    EXPECT_EQ(sampler.NvmlError(), nvml_error_driver_not_loaded);
}

TEST(GpuSamplerTest, SampleBeforeOpenFails)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(1);
    gpu_monitor::GpuSampler sampler{MakeApi(fake)};
    gpu_monitor::GpuUsageMap usage{};
    EXPECT_EQ(sampler.Sample(usage), gpu_monitor::GpuStatus::QueryFailed);
}

TEST(GpuSamplerTest, LeavesOutGpuThatFails)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(2);
    fake->gpus[0].handleCode = nvml_error_gpu_is_lost;
    fake->gpus[1].compute = {ListedProcess(100, mib)};
    gpu_monitor::GpuSampler sampler{MakeApi(fake)};
    gpu_monitor::GpuUsageMap usage{};
    ASSERT_EQ(sampler.Open(), gpu_monitor::GpuStatus::Ok);
    ASSERT_EQ(sampler.Sample(usage), gpu_monitor::GpuStatus::Ok);
    EXPECT_EQ(usage.at(100).memoryBytes, mib);
}

TEST(GpuSamplerTest, EveryGpuFailingReturnsQueryFailed)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(1);
    fake->gpus[0].handleCode = nvml_error_gpu_is_lost;
    gpu_monitor::GpuSampler sampler{MakeApi(fake)};
    gpu_monitor::GpuUsageMap usage{};
    ASSERT_EQ(sampler.Open(), gpu_monitor::GpuStatus::Ok);
    EXPECT_EQ(sampler.Sample(usage), gpu_monitor::GpuStatus::QueryFailed);
    EXPECT_EQ(sampler.NvmlError(), nvml_error_gpu_is_lost);
}

TEST(GpuSamplerTest, ShutsNvmlDownOnceAfterMove)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    {
        gpu_monitor::GpuSampler first{MakeApi(fake)};
        ASSERT_EQ(first.Open(), gpu_monitor::GpuStatus::Ok);
        gpu_monitor::GpuSampler second{std::move(first)};
    }

    EXPECT_EQ(fake->shutdowns.load(), 1);
}

TEST(GpuSamplerTest, UnopenedSamplerDoesNotShutNvmlDown)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    {
        gpu_monitor::GpuSampler sampler{MakeApi(fake)};
    }

    EXPECT_EQ(fake->shutdowns.load(), 0);
}

TEST(GpuMonitorTest, LoadNvmlFillsEveryRequiredCallOrNone)
{
    gpu_monitor::NvmlApi api{};
    const gpu_monitor::GpuStatus status = gpu_monitor::LoadNvml(api);
    ASSERT_TRUE(status == gpu_monitor::GpuStatus::Ok || status == gpu_monitor::GpuStatus::LibraryMissing);
    const bool filled = api.init && api.shutdown && api.deviceCount && api.deviceHandle && api.computeProcesses;
    EXPECT_EQ(filled, status == gpu_monitor::GpuStatus::Ok);
}

TEST(GpuMonitorTest, PublishesSamples)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(1);
    fake->gpus[0].compute = {ListedProcess(100, 512 * mib)};
    gpu_monitor::GpuMonitor monitor{LoaderOf(MakeApi(fake)), fast_period, fast_period};
    EXPECT_TRUE(WaitFor(monitor,
                        [](const gpu_monitor::GpuSnapshot& snapshot)
                        {
                            return snapshot.status == gpu_monitor::GpuStatus::Ok && snapshot.processes.count(100) == 1 &&
                                   snapshot.processes.at(100).memoryBytes == 512 * mib;
                        }));
}

TEST(GpuMonitorTest, ReportsNotSampledBeforeFirstSample)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(1);
    const std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    const gpu_monitor::NvmlApi api = MakeApi(fake);
    gpu_monitor::GpuMonitor monitor{[gate, api](gpu_monitor::NvmlApi& out)
                                    {
                                        PassGate(*gate);
                                        out = api;
                                        return gpu_monitor::GpuStatus::Ok;
                                    },
                                    fast_period, fast_period};
    ASSERT_TRUE(WaitUntil(gate->entered));
    EXPECT_EQ(monitor.Latest().status, gpu_monitor::GpuStatus::NotSampled);
    OpenGate(*gate);
    EXPECT_TRUE(WaitFor(monitor, IsOk));
}

TEST(GpuMonitorTest, ReportsMissingLibrary)
{
    const gpu_monitor::GpuMonitor monitor{[](gpu_monitor::NvmlApi&)
                                          {
                                              return gpu_monitor::GpuStatus::LibraryMissing;
                                          },
                                          fast_period, slow_period};
    EXPECT_TRUE(WaitFor(monitor,
                        [](const gpu_monitor::GpuSnapshot& snapshot)
                        {
                            return snapshot.status == gpu_monitor::GpuStatus::LibraryMissing &&
                                   snapshot.processes.empty();
                        }));
}

TEST(GpuMonitorTest, MovedMonitorKeepsSampling)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(1);
    fake->gpus[0].compute = {ListedProcess(100, mib)};
    gpu_monitor::GpuMonitor first{LoaderOf(MakeApi(fake)), fast_period, fast_period};
    const gpu_monitor::GpuMonitor second{std::move(first)};
    EXPECT_EQ(first.Latest().status, gpu_monitor::GpuStatus::NotSampled);
    EXPECT_TRUE(WaitFor(second,
                        [](const gpu_monitor::GpuSnapshot& snapshot)
                        {
                            return IsOk(snapshot) && snapshot.processes.count(100) == 1;
                        }));
}

TEST(GpuMonitorTest, ReportsInitFailureWithNvmlCode)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->initCode = nvml_error_driver_not_loaded;
    gpu_monitor::GpuMonitor monitor{LoaderOf(MakeApi(fake)), fast_period, slow_period};
    EXPECT_TRUE(WaitFor(monitor,
                        [](const gpu_monitor::GpuSnapshot& snapshot)
                        {
                            return snapshot.status == gpu_monitor::GpuStatus::InitFailed &&
                                   snapshot.nvmlError == nvml_error_driver_not_loaded;
                        }));
}

TEST(GpuMonitorTest, RetriesLoadingNvml)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(1);
    const std::shared_ptr<std::atomic<int>> loads = std::make_shared<std::atomic<int>>(0);
    const gpu_monitor::NvmlApi api = MakeApi(fake);
    gpu_monitor::GpuMonitor monitor{[loads, api](gpu_monitor::NvmlApi& out)
                                    {
                                        if (++*loads == 1)
                                        {
                                            return gpu_monitor::GpuStatus::LibraryMissing;
                                        }

                                        out = api;
                                        return gpu_monitor::GpuStatus::Ok;
                                    },
                                    fast_period, fast_period};
    EXPECT_TRUE(WaitFor(monitor, IsOk));
    EXPECT_GE(loads->load(), 2);
}

TEST(GpuMonitorTest, StopsWithoutWaitingOutThePeriod)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(1);
    std::chrono::steady_clock::time_point stopAt{};
    {
        gpu_monitor::GpuMonitor monitor{LoaderOf(MakeApi(fake)), slow_period, slow_period};
        ASSERT_TRUE(WaitFor(monitor, IsOk));
        stopAt = std::chrono::steady_clock::now();
    }

    EXPECT_LT(std::chrono::steady_clock::now() - stopAt, std::chrono::seconds{1});
    EXPECT_EQ(fake->shutdowns.load(), 1);
}

TEST(GpuMonitorTest, LeavesStuckDriverCallBehind)
{
    const std::shared_ptr<FakeNvml> fake = std::make_shared<FakeNvml>();
    fake->gpus.resize(1);
    const std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    gpu_monitor::NvmlApi api = MakeApi(fake);
    api.deviceCount = [gate](unsigned int& count)
    {
        PassGate(*gate);
        count = 1;
        return gpu_monitor::nvml_success;
    };
    api.shutdown = [gate]()
    {
        gate->shutDown = true;
        return gpu_monitor::nvml_success;
    };

    std::chrono::steady_clock::time_point stopAt{};
    {
        gpu_monitor::GpuMonitor monitor{LoaderOf(api), fast_period, fast_period};
        ASSERT_TRUE(WaitUntil(gate->entered));
        stopAt = std::chrono::steady_clock::now();
    }

    const std::chrono::steady_clock::duration took = std::chrono::steady_clock::now() - stopAt;
    EXPECT_GE(took, std::chrono::seconds{1});
    EXPECT_LT(took, std::chrono::seconds{4});

    OpenGate(*gate);
    EXPECT_TRUE(WaitUntil(gate->shutDown)) << "the thread left behind finishes once the driver call returns";
}
