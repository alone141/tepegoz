// pmt_heartbeat: sends the manager heartbeats as a service, and can start late, stop beating or flap.

#include "TestService.hpp"

#include <chrono>
#include <cxxopts.hpp>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    test_service::SetProgramName("pmt_heartbeat");
    cxxopts::Options options{"pmt_heartbeat", "Sends heartbeats to the manager as --name, its ProcessName."};
    options.add_options()
        ("name", "ProcessName of the service (required)", cxxopts::value<std::string>())
        ("endpoint", "Broker endpoint", cxxopts::value<std::string>()->default_value(std::string{test_service::default_command_endpoint}))
        ("period-ms", "Time between heartbeats", cxxopts::value<int>()->default_value("1000"))
        ("first-beat-ms", "Wait before the first heartbeat", cxxopts::value<int>()->default_value("0"))
        ("beats", "Stop beating after this many and keep running; 0 beats forever", cxxopts::value<int>()->default_value("0"))
        ("flap-ms", "Beat for this long, stay silent for this long, and repeat; 0 does not flap", cxxopts::value<int>()->default_value("0"));
    cxxopts::ParseResult result{};
    const test_service::OptionsCode parsed = test_service::ParseOptions(options, argc, argv, result);
    if (parsed != test_service::OptionsCode::Ok)
    {
        return test_service::ExitCodeOf(parsed);
    }

    if (result.count("name") == 0)
    {
        std::cerr << "--name is required: the manager matches heartbeats by the service's ProcessName.\n";
        return test_service::exit_invalid_options;
    }

    const std::string name = result["name"].as<std::string>();
    const std::chrono::milliseconds period{result["period-ms"].as<int>()};
    const std::chrono::milliseconds firstBeat{result["first-beat-ms"].as<int>()};
    const int beats = result["beats"].as<int>();
    const std::chrono::milliseconds flap{result["flap-ms"].as<int>()};
    test_service::InstallSignalHandlers(test_service::TermMode::Stop);
    test_service::HeartbeatSender sender{name, result["endpoint"].as<std::string>()};
    test_service::Say("started as " + name + "; first heartbeat in " + std::to_string(firstBeat.count()) + " ms");
    if (!test_service::SleepUnlessStopped(firstBeat))
    {
        test_service::Say("stop asked; exiting with 0");
        return 0;
    }

    const auto flapStart = std::chrono::steady_clock::now();
    bool silent = false;
    int sent = 0;
    while (!test_service::StopRequested())
    {
        const bool flapSilent = flap.count() > 0 && ((std::chrono::steady_clock::now() - flapStart) / flap) % 2 == 1;
        const bool nowSilent = flapSilent || (beats > 0 && sent >= beats);
        if (nowSilent != silent)
        {
            silent = nowSilent;
            test_service::Say(silent ? "silent after " + std::to_string(sent) + " heartbeats" : "beating again");
        }

        if (!silent)
        {
            ++sent;
            if (sender.Beat() != test_service::BeatCode::Ok)
            {
                test_service::SayError("heartbeat " + std::to_string(sent) + " dropped");
            }
            else if (sent == 1 || sent % 10 == 0)
            {
                test_service::Say("heartbeat " + std::to_string(sent));
            }
        }

        test_service::SleepUnlessStopped(period);
    }

    test_service::Say("stop asked after " + std::to_string(sent) + " heartbeats; exiting with 0");
    return 0;
}
