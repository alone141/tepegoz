// tpz_hog: keeps threads busy for a share of the time and holds memory, for the CPU and memory figures.

#include "TestService.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cxxopts.hpp>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace test_service
{

namespace
{

constexpr std::chrono::milliseconds duty_window{100};
constexpr std::size_t page_bytes = 4096;

// Spins for percent of every window and sleeps for the rest.
void Burn(int percent)
{
    const auto busy = duty_window * percent / 100;
    while (!StopRequested())
    {
        const auto windowStart = std::chrono::steady_clock::now();
        volatile unsigned long long sink = 0;
        while (std::chrono::steady_clock::now() - windowStart < busy)
        {
            sink = sink + 1;
        }

        SleepUnlessStopped(std::chrono::duration_cast<std::chrono::milliseconds>(duty_window - busy));
    }
}

} // namespace

} // namespace test_service

int main(int argc, char** argv)
{
    test_service::SetProgramName("tpz_hog");
    cxxopts::Options options{"tpz_hog", "Burns CPU and holds memory until SIGTERM."};
    options.add_options()
        ("cpu-percent", "Busy share of each thread, 0 to 100", cxxopts::value<int>()->default_value("50"))
        ("threads", "Busy threads", cxxopts::value<int>()->default_value("1"))
        ("memory-mb", "Memory to allocate and touch", cxxopts::value<int>()->default_value("64"))
        ("report-ms", "Time between status lines", cxxopts::value<int>()->default_value("2000"));
    cxxopts::ParseResult result{};
    const test_service::OptionsCode parsed = test_service::ParseOptions(options, argc, argv, result);
    if (parsed != test_service::OptionsCode::Ok)
    {
        return test_service::ExitCodeOf(parsed);
    }

    const int percent = std::clamp(result["cpu-percent"].as<int>(), 0, 100);
    const int threads = std::max(result["threads"].as<int>(), 0);
    const std::size_t memoryBytes = static_cast<std::size_t>(std::max(result["memory-mb"].as<int>(), 0)) * 1024 * 1024;
    const std::chrono::milliseconds reportPeriod{result["report-ms"].as<int>()};
    test_service::InstallSignalHandlers(test_service::TermMode::Stop);

    // Every page is written, so the memory is resident and not only reserved.
    std::unique_ptr<char[]> memory{new char[memoryBytes > 0 ? memoryBytes : 1]};
    for (std::size_t offset = 0; offset < memoryBytes; offset += test_service::page_bytes)
    {
        memory[offset] = static_cast<char>(offset);
    }

    std::vector<std::thread> burners{};
    for (int i = 0; i < threads; ++i)
    {
        burners.emplace_back(test_service::Burn, percent);
    }

    const std::string status = "holding " + std::to_string(memoryBytes / (1024 * 1024)) + " MB, " +
                               std::to_string(threads) + " threads busy " + std::to_string(percent) + "% of the time";
    test_service::Say("started; " + status);
    while (test_service::SleepUnlessStopped(reportPeriod))
    {
        test_service::Say(status);
    }

    for (std::thread& burner : burners)
    {
        burner.join();
    }

    test_service::Say("stop asked; exiting with 0");
    return 0;
}
