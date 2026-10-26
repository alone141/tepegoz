// pmt_exit: runs for a while, then exits with the code it is given.

#include "TestService.hpp"

#include <chrono>
#include <cxxopts.hpp>
#include <string>

int main(int argc, char** argv)
{
    test_service::SetProgramName("pmt_exit");
    cxxopts::Options options{"pmt_exit", "Runs for --after-ms, then exits with --code. SIGTERM ends it with 0."};
    options.add_options()
        ("code", "Exit code", cxxopts::value<int>()->default_value("1"))
        ("after-ms", "Run time before the exit", cxxopts::value<int>()->default_value("1000"));
    cxxopts::ParseResult result{};
    const test_service::OptionsCode parsed = test_service::ParseOptions(options, argc, argv, result);
    if (parsed != test_service::OptionsCode::Ok)
    {
        return test_service::ExitCodeOf(parsed);
    }

    const int code = result["code"].as<int>();
    const std::chrono::milliseconds after{result["after-ms"].as<int>()};
    test_service::InstallSignalHandlers(test_service::TermMode::Stop);
    test_service::Say("started; exiting with " + std::to_string(code) + " in " + std::to_string(after.count()) + " ms");
    if (!test_service::SleepUnlessStopped(after))
    {
        test_service::Say("stop asked; exiting with 0");
        return 0;
    }

    test_service::Say("exiting with " + std::to_string(code));
    return code;
}
