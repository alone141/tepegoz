// tpz_crash: runs for a while, then dies by the signal it is given.

#include "TestService.hpp"

#include <chrono>
#include <csignal>
#include <cxxopts.hpp>
#include <iostream>
#include <optional>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/resource.h>

namespace test_service
{

namespace
{

struct NamedSignal
{
    std::string_view name;
    int number;
};

constexpr NamedSignal known_signals[] = {
    {"SEGV", SIGSEGV}, {"ABRT", SIGABRT}, {"KILL", SIGKILL}, {"TERM", SIGTERM}, {"HUP", SIGHUP},
    {"INT", SIGINT},   {"PIPE", SIGPIPE}, {"BUS", SIGBUS},   {"FPE", SIGFPE},   {"USR1", SIGUSR1},
};

std::optional<int> SignalNumber(std::string_view name)
{
    if (name.substr(0, 3) == "SIG")
    {
        name.remove_prefix(3);
    }

    for (const NamedSignal& known : known_signals)
    {
        if (known.name == name)
        {
            return known.number;
        }
    }

    return std::nullopt;
}

} // namespace

} // namespace test_service

int main(int argc, char** argv)
{
    test_service::SetProgramName("tpz_crash");
    cxxopts::Options options{"tpz_crash", "Runs for --after-ms, then raises --signal. SIGTERM before that ends it with 0."};
    options.add_options()
        ("signal", "SEGV, ABRT, KILL, TERM, HUP, INT, PIPE, BUS, FPE or USR1", cxxopts::value<std::string>()->default_value("SEGV"))
        ("after-ms", "Run time before the signal", cxxopts::value<int>()->default_value("1000"));
    cxxopts::ParseResult result{};
    const test_service::OptionsCode parsed = test_service::ParseOptions(options, argc, argv, result);
    if (parsed != test_service::OptionsCode::Ok)
    {
        return test_service::ExitCodeOf(parsed);
    }

    const std::string signalName = result["signal"].as<std::string>();
    const std::optional<int> number = test_service::SignalNumber(signalName);
    if (!number.has_value())
    {
        std::cerr << "Unknown signal '" << signalName << "'. Use --help to list them.\n";
        return test_service::exit_invalid_options;
    }

    // No core file for every planned crash.
    const rlimit noCore{0, 0};
    setrlimit(RLIMIT_CORE, &noCore);

    const std::chrono::milliseconds after{result["after-ms"].as<int>()};
    test_service::InstallSignalHandlers(test_service::TermMode::Stop);
    test_service::Say("started; raising SIG" + signalName + " in " + std::to_string(after.count()) + " ms");
    if (!test_service::SleepUnlessStopped(after))
    {
        test_service::Say("stop asked; exiting with 0");
        return 0;
    }

    test_service::Say("raising SIG" + signalName);
    std::signal(*number, SIG_DFL);
    std::raise(*number);
    test_service::Say("still alive after SIG" + signalName + "; exiting with 99");
    return 99;
}
