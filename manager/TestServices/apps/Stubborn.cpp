// tpz_stubborn: ignores SIGTERM, so only SIGKILL stops it; optionally with a child that ignores it too.

#include "TestService.hpp"

#include <chrono>
#include <cxxopts.hpp>
#include <string>
#include <string_view>
#include <unistd.h>

namespace test_service
{

namespace
{

void Run(std::string_view who, std::chrono::milliseconds period)
{
    int reported = 0;
    int tick = 0;
    while (SleepUnlessStopped(tick == 0 ? std::chrono::milliseconds{0} : period))
    {
        ++tick;
        const int received = TermCount();
        if (received != reported)
        {
            reported = received;
            Say(std::string{who} + " ignored SIGTERM #" + std::to_string(received));
        }
        else
        {
            Say(std::string{who} + " tick " + std::to_string(tick));
        }
    }

    Say(std::string{who} + " stopped by SIGINT");
}

} // namespace

} // namespace test_service

int main(int argc, char** argv)
{
    test_service::SetProgramName("tpz_stubborn");
    cxxopts::Options options{"tpz_stubborn", "Counts and ignores SIGTERM; only SIGKILL (or SIGINT) stops it."};
    options.add_options()
        ("child", "Start a child in the same process group that ignores SIGTERM too")
        ("period-ms", "Time between lines", cxxopts::value<int>()->default_value("1000"));
    cxxopts::ParseResult result{};
    const test_service::OptionsCode parsed = test_service::ParseOptions(options, argc, argv, result);
    if (parsed != test_service::OptionsCode::Ok)
    {
        return test_service::ExitCodeOf(parsed);
    }

    const std::chrono::milliseconds period{result["period-ms"].as<int>()};
    test_service::InstallSignalHandlers(test_service::TermMode::Count);
    test_service::Say("started");
    if (result.count("child") != 0)
    {
        const pid_t child = fork();
        if (child == 0)
        {
            test_service::Say("child pid " + std::to_string(getpid()) + " pgid " + std::to_string(getpgrp()));
            test_service::Run("child", period);
            return 0;
        }

        test_service::Say("started child " + std::to_string(child));
    }

    test_service::Run("parent", period);
    return 0;
}
