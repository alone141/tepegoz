#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace status_table
{
struct ServiceSample;
enum class ServiceState;
} // namespace status_table

namespace command_watch
{

// The target that names every service.
constexpr std::string_view all_services = "all";

// What the command line asked the manager to do.
enum class Command
{
    Start,
    Stop,
    Restart,
    Disable,
};

// Where a command stands, from the reports since it was sent.
enum class WatchCode
{
    Waiting,
    Done,
    Failed,
};

/**
 * @brief Find a service in a report.
 * @param report The services in the report.
 * @param name The ProcessName to find.
 * @return The service, or std::nullopt when the report has none of that name.
 */
std::optional<status_table::ServiceSample> FindService(const std::vector<status_table::ServiceSample>& report,
                                                       std::string_view name);

class CommandWatch
{
public:
    /**
     * @brief Follow a command from the last report before it was sent.
     * @param command The command that was sent.
     * @param target The ProcessName it was sent for, or all_services.
     * @param before The services in the last report before it.
     */
    CommandWatch(Command command, std::string_view target, const std::vector<status_table::ServiceSample>& before);

    /**
     * @brief Take a report that came after the command.
     * @param after The services in it.
     * @param lines Receives a line for each service that got where the command sends it, or that will not.
     * @return Waiting while a service is on its way; then Failed if one will not get there, otherwise Done.
     */
    WatchCode Update(const std::vector<status_table::ServiceSample>& after, std::vector<std::string>& lines);

    /**
     * @brief Describe the services still on their way, once the wait is over.
     * @param waited How long the reports were followed.
     * @param lines Receives a line for each service not there yet.
     */
    void DescribeWaiting(std::chrono::seconds waited, std::vector<std::string>& lines) const;

private:
    struct Track
    {
        std::string name;
        int pidBefore = 0;
        bool aliveBefore = false;
        int lastPid = 0;
        status_table::ServiceState lastState{};
        // A process the command started was seen.
        bool started = false;
        bool startingShown = false;
        bool settled = false;
        bool failed = false;
    };

    void Follow(Track& track, const std::optional<status_table::ServiceSample>& sample,
                std::vector<std::string>& lines) const;

    Command command;
    bool all;
    std::vector<Track> tracks;
};

} // namespace command_watch
