// tpz_chatty: writes numbered lines to stdout and stderr at a steady rate, long or without newlines if asked.

#include "TestService.hpp"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cxxopts.hpp>
#include <string>

int main(int argc, char** argv)
{
    test_service::SetProgramName("tpz_chatty");
    cxxopts::Options options{"tpz_chatty", "Writes numbered lines at --rate per second until SIGTERM or --duration-ms."};
    options.add_options()
        ("rate", "Lines per second", cxxopts::value<int>()->default_value("10"))
        ("line-bytes", "Length of each line, newline not counted", cxxopts::value<int>()->default_value("60"))
        ("stderr-every", "Write every Nth line to stderr; 0 never", cxxopts::value<int>()->default_value("0"))
        ("no-newline", "Leave the newline off every line")
        ("duration-ms", "Exit with 0 after this long; 0 runs until SIGTERM", cxxopts::value<int>()->default_value("0"));
    cxxopts::ParseResult result{};
    const test_service::OptionsCode parsed = test_service::ParseOptions(options, argc, argv, result);
    if (parsed != test_service::OptionsCode::Ok)
    {
        return test_service::ExitCodeOf(parsed);
    }

    const int rate = result["rate"].as<int>() > 0 ? result["rate"].as<int>() : 1;
    const std::size_t lineBytes = static_cast<std::size_t>(result["line-bytes"].as<int>());
    const int stderrEvery = result["stderr-every"].as<int>();
    const bool newline = result.count("no-newline") == 0;
    const std::chrono::milliseconds duration{result["duration-ms"].as<int>()};
    const std::chrono::microseconds period{1000000 / rate};
    test_service::InstallSignalHandlers(test_service::TermMode::Stop);
    test_service::Say("started");

    const auto start = std::chrono::steady_clock::now();
    auto next = start;
    int number = 0;
    while (!test_service::StopRequested() && (duration.count() == 0 || std::chrono::steady_clock::now() - start < duration))
    {
        ++number;
        const bool toStderr = stderrEvery > 0 && number % stderrEvery == 0;
        std::string line = std::string{toStderr ? "stderr" : "stdout"} + " line " + std::to_string(number) + ' ';
        if (line.size() < lineBytes)
        {
            line.append(lineBytes - line.size(), 'x');
        }

        if (newline)
        {
            line += '\n';
        }

        std::FILE* stream = toStderr ? stderr : stdout;
        std::fwrite(line.data(), 1, line.size(), stream);
        std::fflush(stream);
        next += period;
        test_service::SleepUnlessStopped(std::chrono::duration_cast<std::chrono::milliseconds>(next - std::chrono::steady_clock::now()));
    }

    if (!newline)
    {
        std::fputc('\n', stdout);
    }

    test_service::Say("wrote " + std::to_string(number) + " lines; exiting with 0");
    return 0;
}
