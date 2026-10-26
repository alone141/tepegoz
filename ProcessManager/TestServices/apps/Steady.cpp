// pmt_steady: prints a line every period and stops on SIGTERM, optionally taking its time.

#include "TestService.hpp"

#include <chrono>
#include <cxxopts.hpp>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
    test_service::SetProgramName("pmt_steady");
    cxxopts::Options options{"pmt_steady", "Prints a line every period and stops cleanly on SIGTERM."};
    options.add_options()
        ("period-ms", "Time between lines", cxxopts::value<int>()->default_value("1000"))
        ("stop-delay-ms", "Time it takes to stop after SIGTERM", cxxopts::value<int>()->default_value("0"))
        ("exit-code", "Exit code after a stop", cxxopts::value<int>()->default_value("0"));
    cxxopts::ParseResult result{};
    const test_service::OptionsCode parsed = test_service::ParseOptions(options, argc, argv, result);
    if (parsed != test_service::OptionsCode::Ok)
    {
        return test_service::ExitCodeOf(parsed);
    }

    const std::chrono::milliseconds period{result["period-ms"].as<int>()};
    const std::chrono::milliseconds stopDelay{result["stop-delay-ms"].as<int>()};
    const int exitCode = result["exit-code"].as<int>();
    test_service::InstallSignalHandlers(test_service::TermMode::Stop);
    test_service::Say("started");

    int tick = 0;
    while (test_service::SleepUnlessStopped(tick == 0 ? std::chrono::milliseconds{0} : period))
    {
        ++tick;
        test_service::Say("tick " + std::to_string(tick));
    }

    test_service::Say("stop asked; stopping in " + std::to_string(stopDelay.count()) + " ms");
    std::this_thread::sleep_for(stopDelay);
    test_service::Say("exiting with " + std::to_string(exitCode));
    return exitCode;
}
