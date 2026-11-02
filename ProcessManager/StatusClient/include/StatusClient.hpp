#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace command_watch
{
enum class Command;
} // namespace command_watch

namespace status_table
{
struct ServiceSample;
} // namespace status_table

namespace status_client
{

constexpr std::string_view default_reports_endpoint = "tcp://localhost:6667";
constexpr std::string_view default_commands_endpoint = "tcp://localhost:5557";
// A manager publishes a report every 500 ms, so a first one comes well within this.
constexpr std::chrono::milliseconds first_report_timeout{3000};
// How long one table is followed for, so each service's CPU figure spans two reports.
constexpr std::chrono::milliseconds cpu_sample_period{1000};
// Without a report for this long, the table says so.
constexpr std::chrono::seconds stale_after{5};
constexpr std::chrono::milliseconds refresh_period{1000};
constexpr std::chrono::seconds default_command_timeout{10};
// The command line's identity at the broker; the manager takes commands from it.
constexpr std::string_view command_line_identity = "PMC";

// Where the manager's health reports and its broker are.
struct Endpoints
{
    std::string reports{default_reports_endpoint};
    std::string commands{default_commands_endpoint};
};

enum class ParseCode
{
    Ok,
    BadSize,
};

enum class StatusCode
{
    Ok,
    NoReport,
};

enum class CommandCode
{
    Done,
    Failed,
    NoSuchService,
    BadTarget,
    NotReachable,
    TimedOut,
};

/**
 * @brief Decode a health report message: one 144-byte record per service.
 * @param bytes The message.
 * @param out Receives the services on success.
 * @return ParseCode::Ok, or BadSize for a message that is not whole records.
 */
ParseCode ParseReport(std::string_view bytes, std::vector<status_table::ServiceSample>& out);

// Runs until the process is interrupted (Ctrl+C), so it returns nothing.
/**
 * @brief Show every service in a table redrawn each second, from the manager's health reports.
 * @param endpoints Where the reports come from.
 */
void WatchStatus(const Endpoints& endpoints);

/**
 * @brief Print one table of every service, its CPU figures taken over cpu_sample_period.
 * @param endpoints Where the reports come from.
 * @return StatusCode::Ok, or NoReport when none came within first_report_timeout.
 */
StatusCode PrintStatus(const Endpoints& endpoints);

/**
 * @brief Send a command to the manager and, when following it, print what it does to each service.
 * @param command The command to send.
 * @param target A ProcessName, or command_watch::all_services.
 * @param endpoints Where the reports come from and the commands go.
 * @param follow Follow the reports after sending; without it the command is sent and nothing printed.
 * @param timeout How long the reports are followed.
 * @return CommandCode::Done when every service got where the command sends it, otherwise the failure.
 */
CommandCode SendCommand(command_watch::Command command, std::string_view target, const Endpoints& endpoints,
                        bool follow, std::chrono::seconds timeout);

/**
 * @brief Turn a command's outcome into the command line's exit code.
 * @param code The outcome.
 * @return 0 for Done, 2 for TimedOut, otherwise 1.
 */
int ExitCode(CommandCode code);

} // namespace status_client
