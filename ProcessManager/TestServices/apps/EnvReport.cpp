// pmt_env: prints what it was started with (arguments, working directory, ids, environment), then waits.

#include "TestService.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cxxopts.hpp>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace test_service
{

namespace
{

void SayVariable(const std::string& name)
{
    const char* value = std::getenv(name.c_str());
    Say(value == nullptr ? "env " + name + " unset" : "env " + name + "='" + value + "'");
}

} // namespace

} // namespace test_service

int main(int argc, char** argv)
{
    test_service::SetProgramName("pmt_env");
    // The arguments first, as received: they are what is being tested.
    test_service::Say("argc " + std::to_string(argc));
    for (int i = 1; i < argc; ++i)
    {
        test_service::Say("argv[" + std::to_string(i) + "]='" + argv[i] + "'");
    }

    cxxopts::Options options{"pmt_env", "Prints its arguments, working directory, ids and environment, then waits."};
    options.allow_unrecognised_options();
    options.add_options()
        ("show", "Also print this variable; can be repeated", cxxopts::value<std::vector<std::string>>())
        ("all-env", "Print every variable")
        ("exit-after-ms", "Exit with 0 after this long; 0 waits for SIGTERM", cxxopts::value<int>()->default_value("0"));
    cxxopts::ParseResult result{};
    const test_service::OptionsCode parsed = test_service::ParseOptions(options, argc, argv, result);
    if (parsed != test_service::OptionsCode::Ok)
    {
        return test_service::ExitCodeOf(parsed);
    }

    std::error_code error{};
    const std::filesystem::path cwd = std::filesystem::current_path(error);
    test_service::Say("cwd " + (error ? "unknown (" + error.message() + ")" : cwd.string()));
    test_service::Say("pid " + std::to_string(getpid()) + " ppid " + std::to_string(getppid()) + " pgid " +
                      std::to_string(getpgrp()) + " sid " + std::to_string(getsid(0)) + " uid " + std::to_string(getuid()));

    std::vector<std::string> names{"PATH", "HOME", "DISPLAY", "XAUTHORITY", "JOURNAL_STREAM"};
    if (result.count("show") != 0)
    {
        const std::vector<std::string> shown = result["show"].as<std::vector<std::string>>();
        names.insert(names.end(), shown.begin(), shown.end());
    }

    for (const std::string& name : names)
    {
        test_service::SayVariable(name);
    }

    if (result.count("all-env") != 0)
    {
        std::vector<std::string> all{};
        for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry)
        {
            all.emplace_back(*entry);
        }

        std::sort(all.begin(), all.end());
        test_service::Say("environment has " + std::to_string(all.size()) + " variables");
        for (const std::string& entry : all)
        {
            test_service::Say("environ " + entry);
        }
    }

    test_service::InstallSignalHandlers(test_service::TermMode::Stop);
    const std::chrono::milliseconds exitAfter{result["exit-after-ms"].as<int>()};
    test_service::SleepUnlessStopped(exitAfter.count() > 0 ? exitAfter : std::chrono::hours{24 * 365});
    test_service::Say("exiting with 0");
    return 0;
}
