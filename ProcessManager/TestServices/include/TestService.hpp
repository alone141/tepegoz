#pragma once

#include <chrono>
#include <memory>
#include <string_view>

namespace cxxopts
{
class Options;
class ParseResult;
} // namespace cxxopts

namespace test_service
{

// Where the broker takes heartbeats, and the manager's identity behind it.
constexpr std::string_view default_command_endpoint = "tcp://localhost:5557";
constexpr std::string_view manager_id = "BPM";
// Where the manager publishes its detailed health reports.
constexpr std::string_view default_health_endpoint = "tcp://localhost:6667";
constexpr int exit_invalid_options = 2;
// How often a sleep checks whether a stop was asked for.
constexpr std::chrono::milliseconds stop_poll_period{20};

enum class TermMode
{
    Stop,
    Count,
};

enum class OptionsCode
{
    Ok,
    Help,
    Invalid,
};

enum class BeatCode
{
    Ok,
    Dropped,
};

/**
 * @brief Set the name that starts every line the program prints.
 * @param name Program name, such as pmt_steady.
 */
void SetProgramName(std::string_view name);

/**
 * @brief Print one line on stdout as "HH:MM:SS.mmm name[pid]: text" and flush it.
 * @param text Line without its newline.
 */
void Say(std::string_view text);

/**
 * @brief Print one line on stderr in the format of Say() and flush it.
 * @param text Line without its newline.
 */
void SayError(std::string_view text);

/**
 * @brief Route SIGTERM and SIGINT to flags the program polls.
 * @param mode Stop: both ask the program to stop. Count: SIGTERM is only counted, SIGINT still asks to stop.
 */
void InstallSignalHandlers(TermMode mode);

/**
 * @brief Tell whether SIGTERM or SIGINT asked the program to stop.
 * @return true once a stop was asked for.
 */
bool StopRequested();

/**
 * @brief Return how many SIGTERMs arrived since InstallSignalHandlers().
 * @return Number of SIGTERMs received.
 */
int TermCount();

/**
 * @brief Sleep for @p duration, waking early when a stop is asked for.
 * @param duration Time to sleep.
 * @return true when the whole time passed, false when a stop was asked for first.
 */
bool SleepUnlessStopped(std::chrono::milliseconds duration);

/**
 * @brief Add --help to @p options, parse the command line, and print the help or the error.
 * @param options The program's options.
 * @param argc Argument count from main.
 * @param argv Arguments from main.
 * @param out Receives the parsed options on success.
 * @return OptionsCode::Ok to run, Help after printing the help, Invalid after printing the error.
 */
OptionsCode ParseOptions(cxxopts::Options& options, int argc, char** argv, cxxopts::ParseResult& out);

/**
 * @brief Return the exit code for a command line that does not run the program.
 * @param code Result of ParseOptions().
 * @return 0 for Help, exit_invalid_options for Invalid and Ok.
 */
int ExitCodeOf(OptionsCode code);

class HeartbeatSender
{
public:
    /**
     * @brief Connect to the broker as @p name, which must be the service's ProcessName.
     * @param name Identity the manager knows the service by.
     * @param endpoint Broker endpoint, normally default_command_endpoint.
     */
    HeartbeatSender(std::string_view name, std::string_view endpoint);

    /**
     * @brief Close the connection without waiting for heartbeats still queued.
     */
    ~HeartbeatSender();

    HeartbeatSender(const HeartbeatSender&) = delete;
    HeartbeatSender& operator=(const HeartbeatSender&) = delete;

    /**
     * @brief Take over the connection of @p other.
     * @param other Sender to move from. It can no longer send.
     */
    HeartbeatSender(HeartbeatSender&& other) noexcept;

    /**
     * @brief Close this connection and take over the one of @p other.
     * @param other Sender to move from. It can no longer send.
     * @return This sender.
     */
    HeartbeatSender& operator=(HeartbeatSender&& other) noexcept;

    /**
     * @brief Send one heartbeat to the manager without waiting.
     * @return BeatCode::Ok when queued, Dropped when the connection could not take it.
     */
    BeatCode Beat();

private:
    struct Connection;

    std::unique_ptr<Connection> connection;
};

} // namespace test_service
