#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace status_table
{

// Two reports closer together than this give a CPU figure that is mostly noise: the last figure stays.
constexpr std::chrono::milliseconds min_cpu_interval{300};
// The manager sets the last-seen time at the start too. One this close to the start is no heartbeat.
constexpr std::chrono::milliseconds heartbeat_after_start{1};
constexpr int gpu_percent_unknown = -1;
constexpr std::string_view no_value = "-";
// After the name of a service whose reloaded definition waits for its next start.
constexpr std::string_view restart_mark = " *";
// Under the table when a service is marked.
constexpr std::string_view restart_note =
    "* restart to apply: a reloaded definition takes effect at the service's next start";

// The manager's RuntimeState, value for value.
enum class ServiceState
{
    Unknown,
    Starting,
    Running,
    Stopped,
    Unhealthy,
};

// The manager's GpuUsageState, value for value.
enum class GpuState
{
    NotSampled,
    Sampled,
    NoDriver,
    NoDevice,
    Failed,
    Stalled,
};

// One service in one health report. Times are wall-clock nanoseconds since the epoch; 0 means never.
struct ServiceSample
{
    std::string name;
    int pid = 0;
    ServiceState state = ServiceState::Unknown;
    int missedBeats = 0;
    int restarts = 0;
    std::uint64_t cpuUsec = 0;
    std::uint64_t memoryBytes = 0;
    std::int64_t startNs = 0;
    std::int64_t lastSeenNs = 0;
    std::int64_t snapshotNs = 0;
    GpuState gpuState = GpuState::NotSampled;
    int gpuPercent = gpu_percent_unknown;
    std::uint64_t vramBytes = 0;
    // A reloaded definition waits for the service's next start.
    bool restartToApply = false;
};

// How a table is drawn.
struct TableOptions
{
    // First in the header line: the manager's machine.
    std::string machine;
    // Last in the header line, highlighted, such as "No report for 7s"; empty for none.
    std::string notice;
    // ANSI colours for the states and the notice.
    bool color = false;
};

/**
 * @brief Name a state as the manager's log does.
 * @param state The state to name.
 * @return "Unknown", "Starting", "Running", "Stopped" or "Unhealthy".
 */
std::string_view StateName(ServiceState state);

/**
 * @brief Tell whether a state has a live process.
 * @param state The state to check.
 * @return true for Starting, Running and Unhealthy.
 */
bool IsAlive(ServiceState state);

/**
 * @brief Format a duration as the monitor GUI does: "45.2s", "31:51" or "02:14:05".
 * @param duration The duration. A negative one has no value.
 * @return The text, or no_value for a negative duration.
 */
std::string FormatDuration(std::chrono::nanoseconds duration);

/**
 * @brief Format a size as the monitor GUI does: "0 B", "512 B" or "214.1 MB".
 * @param bytes The size in bytes.
 * @return The text, in the largest unit below 1024 of it.
 */
std::string FormatBytes(std::uint64_t bytes);

class StatusTable
{
public:
    /**
     * @brief Take the newest report: every service at one moment.
     * @param report The services in it. A service's CPU figure needs the one before it from the same process.
     */
    void Update(const std::vector<ServiceSample>& report);

    /**
     * @brief Draw the newest report: a header line, then a row per service, then restart_note when one is marked.
     * @param options The machine, a notice and whether to colour.
     * @return The table, one line per row, each ending in a newline.
     */
    std::string Render(const TableOptions& options) const;

    /**
     * @brief Return a service's CPU use in percent of one core.
     * @param name The service's ProcessName.
     * @return The figure, or std::nullopt before two reports of the same process.
     */
    std::optional<double> CpuPercent(std::string_view name) const;

    /**
     * @brief Return the services of the newest report.
     * @return The services, empty before the first report.
     */
    const std::vector<ServiceSample>& Report() const;

private:
    struct CpuBase
    {
        int pid = 0;
        std::uint64_t cpuUsec = 0;
        std::int64_t snapshotNs = 0;
    };

    std::vector<ServiceSample> report;
    std::unordered_map<std::string, CpuBase> bases;
    std::unordered_map<std::string, double> cpu;
};

} // namespace status_table
