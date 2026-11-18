#include "TestService.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <cxxopts.hpp>
#include <iostream>
#include <memory>
#include <signal.h>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <zmq.hpp>

namespace test_service
{

namespace
{

volatile std::sig_atomic_t stopRequested = 0;
volatile std::sig_atomic_t termCount = 0;
std::string programName = "tpz";

void OnStopSignal(int)
{
    stopRequested = 1;
}

void OnCountedTerm(int)
{
    termCount = termCount + 1;
}

std::string FormatLine(std::string_view text)
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local{};
    localtime_r(&seconds, &local);
    char stamp[16]{};
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);
    char prefix[64]{};
    std::snprintf(prefix, sizeof(prefix), "%s.%03d ", stamp, static_cast<int>(millis));
    std::string line{prefix};
    line += programName;
    line += '[';
    line += std::to_string(getpid());
    line += "]: ";
    line += text;
    line += '\n';
    return line;
}

void WriteLine(std::FILE* stream, std::string_view text)
{
    const std::string line = FormatLine(text);
    std::fwrite(line.data(), 1, line.size(), stream);
    std::fflush(stream);
}

void Install(int signal, void (*handler)(int))
{
    struct sigaction action{};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    sigaction(signal, &action, nullptr);
}

} // namespace

void SetProgramName(std::string_view name)
{
    programName = std::string{name};
}

void Say(std::string_view text)
{
    WriteLine(stdout, text);
}

void SayError(std::string_view text)
{
    WriteLine(stderr, text);
}

void InstallSignalHandlers(TermMode mode)
{
    Install(SIGINT, OnStopSignal);
    Install(SIGTERM, mode == TermMode::Stop ? OnStopSignal : OnCountedTerm);
}

bool StopRequested()
{
    return stopRequested != 0;
}

int TermCount()
{
    return termCount;
}

bool SleepUnlessStopped(std::chrono::milliseconds duration)
{
    const auto until = std::chrono::steady_clock::now() + duration;
    while (!StopRequested())
    {
        const auto left = until - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero())
        {
            return true;
        }

        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(left, stop_poll_period));
    }

    return false;
}

OptionsCode ParseOptions(cxxopts::Options& options, int argc, char** argv, cxxopts::ParseResult& out)
{
    options.add_options()("h,help", "Show this help and exit");
    try
    {
        out = options.parse(argc, argv);
    }
    catch (const cxxopts::exceptions::exception& e)
    {
        std::cerr << e.what() << ". Use --help to list the options.\n";
        return OptionsCode::Invalid;
    }

    if (out.count("help") != 0)
    {
        std::cout << options.help() << '\n';
        return OptionsCode::Help;
    }

    return OptionsCode::Ok;
}

int ExitCodeOf(OptionsCode code)
{
    return code == OptionsCode::Help ? 0 : exit_invalid_options;
}

struct HeartbeatSender::Connection
{
    zmq::context_t context{1};
    zmq::socket_t socket{context, zmq::socket_type::dealer};
    std::string name;
};

HeartbeatSender::HeartbeatSender(std::string_view name, std::string_view endpoint)
    : connection{std::make_unique<Connection>()}
{
    connection->name = std::string{name};
    connection->socket.set(zmq::sockopt::routing_id, connection->name);
    connection->socket.set(zmq::sockopt::linger, 0);
    connection->socket.connect(std::string{endpoint});
}

HeartbeatSender::~HeartbeatSender() = default;

HeartbeatSender::HeartbeatSender(HeartbeatSender&& other) noexcept = default;

HeartbeatSender& HeartbeatSender::operator=(HeartbeatSender&& other) noexcept = default;

BeatCode HeartbeatSender::Beat()
{
    if (!connection)
    {
        return BeatCode::Dropped;
    }

    // [BPM][name]: the broker routes it to the manager, which takes any message from a service's name as a beat.
    const auto routed = connection->socket.send(zmq::buffer(manager_id), zmq::send_flags::sndmore | zmq::send_flags::dontwait);
    if (!routed.has_value())
    {
        return BeatCode::Dropped;
    }

    const auto sent = connection->socket.send(zmq::buffer(connection->name), zmq::send_flags::dontwait);
    return sent.has_value() ? BeatCode::Ok : BeatCode::Dropped;
}

} // namespace test_service
