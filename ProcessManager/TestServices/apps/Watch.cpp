// pmt_watch: prints the manager's detailed health reports as text, one line per service, for scripts and people.

#include "TestService.hpp"

#include "RuntimeHealth.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cxxopts.hpp>
#include <string>
#include <zmq.hpp>

namespace test_service
{

namespace
{

long long EpochMs(std::chrono::high_resolution_clock::time_point time)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

void PrintReports(const zmq::message_t& message)
{
    const std::size_t size = sizeof(process_manager::DetailedHealthReport);
    if (message.size() % size != 0)
    {
        std::printf("bad report of %zu bytes\n", message.size());
        std::fflush(stdout);
        return;
    }

    const long long now = EpochMs(std::chrono::high_resolution_clock::now());
    const std::size_t count = message.size() / size;
    std::printf("report %lld %zu\n", now, count);
    for (std::size_t i = 0; i < count; ++i)
    {
        process_manager::DetailedHealthReport report{};
        std::memcpy(&report, static_cast<const char*>(message.data()) + i * size, size);
        report.processName[sizeof(report.processName) - 1] = '\0';
        const process_manager::RuntimeHealth& health = report.runtimeHealth;
        std::printf("service pid=%d state=%s restarts=%d missed=%d cpu_us=%llu mem=%llu started_ms=%lld name=%s\n",
                    static_cast<int>(report.pid), process_manager::RuntimeStateToString(health.state), health.restartCount,
                    health.missedBeats, static_cast<unsigned long long>(report.cpuUsageInUsec),
                    static_cast<unsigned long long>(report.memoryUsageInBytes), EpochMs(health.startTime),
                    report.processName);
    }

    std::fflush(stdout);
}

} // namespace

} // namespace test_service

int main(int argc, char** argv)
{
    test_service::SetProgramName("pmt_watch");
    cxxopts::Options options{"pmt_watch", "Prints the manager's health reports: a 'report <epoch ms> <count>' line, "
                                          "then one 'service ...' line per service."};
    options.add_options()
        ("endpoint", "Health report endpoint", cxxopts::value<std::string>()->default_value(std::string{test_service::default_health_endpoint}))
        ("once", "Exit after the first report")
        ("timeout-ms", "Exit with 1 when no report arrives for this long; 0 waits forever", cxxopts::value<int>()->default_value("0"));
    cxxopts::ParseResult result{};
    const test_service::OptionsCode parsed = test_service::ParseOptions(options, argc, argv, result);
    if (parsed != test_service::OptionsCode::Ok)
    {
        return test_service::ExitCodeOf(parsed);
    }

    const bool once = result.count("once") != 0;
    const int timeoutMs = result["timeout-ms"].as<int>();
    test_service::InstallSignalHandlers(test_service::TermMode::Stop);
    zmq::context_t context{1};
    zmq::socket_t subscriber{context, zmq::socket_type::sub};
    subscriber.set(zmq::sockopt::linger, 0);
    subscriber.set(zmq::sockopt::subscribe, "");
    subscriber.connect(result["endpoint"].as<std::string>());

    auto lastReport = std::chrono::steady_clock::now();
    while (!test_service::StopRequested())
    {
        zmq::pollitem_t items[] = {{subscriber.handle(), 0, ZMQ_POLLIN, 0}};
        zmq::poll(items, 1, test_service::stop_poll_period * 10);
        if ((items[0].revents & ZMQ_POLLIN) != 0)
        {
            zmq::message_t message{};
            if (subscriber.recv(message, zmq::recv_flags::dontwait).has_value())
            {
                lastReport = std::chrono::steady_clock::now();
                test_service::PrintReports(message);
                if (once)
                {
                    return 0;
                }
            }
        }

        if (timeoutMs > 0 && std::chrono::steady_clock::now() - lastReport > std::chrono::milliseconds{timeoutMs})
        {
            std::fprintf(stderr, "no report for %d ms\n", timeoutMs);
            return 1;
        }
    }

    return 0;
}
