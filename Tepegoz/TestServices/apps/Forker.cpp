// tpz_forker: starts children in its process group and, optionally, one that escapes into a session of its own;
// the parent can crash and leave them behind.

#include "TestService.hpp"

#include <chrono>
#include <csignal>
#include <cxxopts.hpp>
#include <signal.h>
#include <string>
#include <sys/resource.h>
#include <unistd.h>

namespace test_service
{

namespace
{

// A child ticks until it is killed or its lifetime runs out, so a leftover does not live forever.
void RunChild(const std::string& who, std::chrono::milliseconds period, std::chrono::milliseconds lifetime)
{
    std::signal(SIGTERM, SIG_DFL);
    std::signal(SIGINT, SIG_DFL);
    Say(who + " pid " + std::to_string(getpid()) + " pgid " + std::to_string(getpgrp()) + " sid " +
        std::to_string(getsid(0)));
    const auto until = std::chrono::steady_clock::now() + lifetime;
    int tick = 0;
    while (std::chrono::steady_clock::now() < until)
    {
        SleepUnlessStopped(period);
        ++tick;
        Say(who + " tick " + std::to_string(tick) + " parent " + std::to_string(getppid()));
    }

    Say(who + " lifetime over; exiting with 0");
}

} // namespace

} // namespace test_service

int main(int argc, char** argv)
{
    test_service::SetProgramName("tpz_forker");
    cxxopts::Options options{"tpz_forker", "Starts children; the parent stops on SIGTERM and leaves them to the manager."};
    options.add_options()
        ("children", "Children in the parent's process group", cxxopts::value<int>()->default_value("2"))
        ("escapee", "Also start a child that calls setsid(), leaving the process group")
        ("crash-parent-after-ms", "Crash the parent with SIGSEGV after this long; 0 never", cxxopts::value<int>()->default_value("0"))
        ("period-ms", "Time between the children's lines", cxxopts::value<int>()->default_value("2000"))
        ("child-lifetime-ms", "A child exits on its own after this long", cxxopts::value<int>()->default_value("300000"));
    cxxopts::ParseResult result{};
    const test_service::OptionsCode parsed = test_service::ParseOptions(options, argc, argv, result);
    if (parsed != test_service::OptionsCode::Ok)
    {
        return test_service::ExitCodeOf(parsed);
    }

    const int children = result["children"].as<int>();
    const bool escapee = result.count("escapee") != 0;
    const std::chrono::milliseconds crashAfter{result["crash-parent-after-ms"].as<int>()};
    const std::chrono::milliseconds period{result["period-ms"].as<int>()};
    const std::chrono::milliseconds lifetime{result["child-lifetime-ms"].as<int>()};

    // Children are reaped as they exit, and a planned crash leaves no core file.
    std::signal(SIGCHLD, SIG_IGN);
    const rlimit noCore{0, 0};
    setrlimit(RLIMIT_CORE, &noCore);
    test_service::Say("parent pid " + std::to_string(getpid()) + " pgid " + std::to_string(getpgrp()));

    for (int i = 1; i <= children + (escapee ? 1 : 0); ++i)
    {
        const bool escaping = i > children;
        const pid_t child = fork();
        if (child == 0)
        {
            if (escaping)
            {
                setsid();
            }

            test_service::RunChild(escaping ? "escapee" : "child " + std::to_string(i), period, lifetime);
            return 0;
        }

        test_service::Say("started " + std::string{escaping ? "escapee " : "child "} + std::to_string(child));
    }

    test_service::InstallSignalHandlers(test_service::TermMode::Stop);
    const bool crash = crashAfter.count() > 0;
    if (test_service::SleepUnlessStopped(crash ? crashAfter : std::chrono::hours{24 * 365}))
    {
        test_service::Say("parent crashing with SIGSEGV");
        std::signal(SIGSEGV, SIG_DFL);
        std::raise(SIGSEGV);
    }

    test_service::Say("stop asked; parent exiting with 0");
    return 0;
}
