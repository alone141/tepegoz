#include "StatusTable.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <utility>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace status_table
{

namespace
{

constexpr std::string_view color_reset = "\033[0m";
constexpr std::string_view color_green = "\033[32m";
constexpr std::string_view color_yellow = "\033[33m";
constexpr std::string_view color_red = "\033[31m";
constexpr std::string_view color_dim = "\033[2m";
constexpr std::string_view color_magenta = "\033[35m";
constexpr std::string_view column_gap = "  ";
constexpr std::string_view header_separator = " | ";
constexpr std::int64_t nanoseconds_per_second = 1000000000;

enum class Align
{
    Left,
    Right,
};

struct Column
{
    std::string_view title;
    Align align = Align::Left;
};

// A cell's text, and the colour it is drawn in when colours are on.
struct Cell
{
    std::string text;
    std::string_view color;
};

std::string_view StateColor(ServiceState state)
{
    switch (state)
    {
    case ServiceState::Running:
        return color_green;
    case ServiceState::Starting:
        return color_yellow;
    case ServiceState::Unhealthy:
        return color_red;
    case ServiceState::Stopped:
        return color_dim;
    case ServiceState::Unknown:
        return color_magenta;
    }
    return {};
}

std::string_view GpuText(GpuState state)
{
    switch (state)
    {
    case GpuState::Sampled:
        return "GPU ok";
    case GpuState::NoDriver:
        return "no GPU driver";
    case GpuState::NoDevice:
        return "no GPU";
    case GpuState::Failed:
        return "GPU sampling failed";
    case GpuState::Stalled:
        return "GPU sampling stalled";
    case GpuState::NotSampled:
        return "GPU not sampled yet";
    }
    return "GPU unknown";
}

std::string Painted(std::string_view text, std::string_view color, bool on)
{
    std::string out{};
    if (on && !color.empty())
    {
        out.append(color);
        out.append(text);
        out.append(color_reset);
        return out;
    }

    out.append(text);
    return out;
}

// Pads by the text's own length, so colour codes do not shift the columns.
std::string Padded(const Cell& cell, std::size_t width, Align align, bool color)
{
    const std::size_t fill = width > cell.text.size() ? width - cell.text.size() : 0;
    const std::string spaces(fill, ' ');
    const std::string painted = Painted(cell.text, cell.color, color);
    return align == Align::Left ? painted + spaces : spaces + painted;
}

std::string OneDecimal(double value)
{
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    return std::string{buffer};
}

std::string ClockText(std::int64_t ns)
{
    if (ns <= 0)
    {
        return std::string{no_value};
    }

    const std::time_t seconds = static_cast<std::time_t>(ns / nanoseconds_per_second);
    std::tm local{};
    ::localtime_r(&seconds, &local);
    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    return std::string{buffer};
}

Cell HeartbeatCell(const ServiceSample& sample)
{
    if (!IsAlive(sample.state) || sample.pid <= 0)
    {
        return Cell{std::string{no_value}, {}};
    }

    if (sample.missedBeats > 0)
    {
        return Cell{std::to_string(sample.missedBeats) + " missed", color_red};
    }

    const std::chrono::nanoseconds sinceStart{sample.lastSeenNs - sample.startNs};
    if (sample.lastSeenNs <= 0 || sinceStart < heartbeat_after_start)
    {
        return Cell{std::string{no_value}, {}};
    }

    return Cell{FormatDuration(std::chrono::nanoseconds{sample.snapshotNs - sample.lastSeenNs}) + " ago", {}};
}

std::string CountsText(const std::vector<ServiceSample>& report)
{
    if (report.empty())
    {
        return "no services";
    }

    std::string out = std::to_string(report.size()) + (report.size() == 1 ? " service:" : " services:");
    bool first = true;
    for (const ServiceState state : {ServiceState::Running, ServiceState::Starting, ServiceState::Unhealthy,
                                     ServiceState::Stopped, ServiceState::Unknown})
    {
        const std::ptrdiff_t count = std::count_if(
            report.begin(), report.end(), [state](const ServiceSample& sample) { return sample.state == state; });
        if (count == 0)
        {
            continue;
        }

        std::string name{StateName(state)};
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        out += (first ? " " : ", ") + std::to_string(count) + " " + name;
        first = false;
    }
    return out;
}

std::string HeaderLine(const std::vector<ServiceSample>& report, const TableOptions& options)
{
    std::string out = options.machine.empty() ? std::string{"process manager"} : options.machine;
    out.append(header_separator);
    out.append(CountsText(report));
    if (!report.empty())
    {
        std::int64_t newest = 0;
        for (const ServiceSample& sample : report)
        {
            newest = std::max(newest, sample.snapshotNs);
        }
        out.append(header_separator);
        out.append(GpuText(report.front().gpuState));
        out.append(header_separator);
        out.append(ClockText(newest));
    }
    if (!options.notice.empty())
    {
        out.append(header_separator);
        out.append(Painted(options.notice, color_red, options.color));
    }
    return out;
}

} // namespace

std::string_view StateName(ServiceState state)
{
    switch (state)
    {
    case ServiceState::Unknown:
        return "Unknown";
    case ServiceState::Starting:
        return "Starting";
    case ServiceState::Running:
        return "Running";
    case ServiceState::Stopped:
        return "Stopped";
    case ServiceState::Unhealthy:
        return "Unhealthy";
    }
    return "Unknown";
}

bool IsAlive(ServiceState state)
{
    return state == ServiceState::Starting || state == ServiceState::Running || state == ServiceState::Unhealthy;
}

std::string FormatDuration(std::chrono::nanoseconds duration)
{
    if (duration.count() < 0)
    {
        return std::string{no_value};
    }

    char buffer[32]{};
    const double seconds = std::chrono::duration<double>{duration}.count();
    if (seconds < 60.0)
    {
        std::snprintf(buffer, sizeof(buffer), "%.1fs", seconds);
        return std::string{buffer};
    }

    const long long total = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    const long long hours = total / 3600;
    const long long minutes = total / 60 % 60;
    const long long rest = total % 60;
    if (hours > 0)
    {
        std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld", hours, minutes, rest);
        return std::string{buffer};
    }

    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld", minutes, rest);
    return std::string{buffer};
}

std::string FormatBytes(std::uint64_t bytes)
{
    if (bytes < 1024)
    {
        return std::to_string(bytes) + " B";
    }

    double value = static_cast<double>(bytes);
    std::string_view unit{};
    for (const std::string_view next : {"KB", "MB", "GB", "TB"})
    {
        value /= 1024.0;
        unit = next;
        if (value < 1024.0)
        {
            break;
        }
    }
    return OneDecimal(value) + " " + std::string{unit};
}

void StatusTable::Update(const std::vector<ServiceSample>& report)
{
    for (const ServiceSample& sample : report)
    {
        if (!IsAlive(sample.state) || sample.pid <= 0)
        {
            bases.erase(sample.name);
            cpu.erase(sample.name);
            continue;
        }

        CpuBase& base = bases[sample.name];
        if (base.pid != sample.pid || sample.cpuUsec < base.cpuUsec)
        {
            // A new process, or a counter that went back: its figure starts from here.
            base = CpuBase{sample.pid, sample.cpuUsec, sample.snapshotNs};
            cpu.erase(sample.name);
            continue;
        }

        const std::chrono::nanoseconds elapsed{sample.snapshotNs - base.snapshotNs};
        if (elapsed < min_cpu_interval)
        {
            continue;
        }

        const double usedUsec = static_cast<double>(sample.cpuUsec - base.cpuUsec);
        const double elapsedUsec = std::chrono::duration<double, std::micro>{elapsed}.count();
        cpu[sample.name] = usedUsec / elapsedUsec * 100.0;
        base = CpuBase{sample.pid, sample.cpuUsec, sample.snapshotNs};
    }

    // A service that left the reports leaves its figures behind.
    std::vector<std::string> gone{};
    for (const std::pair<const std::string, CpuBase>& entry : bases)
    {
        const bool reported = std::any_of(report.begin(), report.end(),
                                          [&entry](const ServiceSample& sample) { return sample.name == entry.first; });
        if (!reported)
        {
            gone.push_back(entry.first);
        }
    }
    for (const std::string& name : gone)
    {
        bases.erase(name);
        cpu.erase(name);
    }
    this->report = report;
}

std::string StatusTable::Render(const TableOptions& options) const
{
    std::string out = HeaderLine(report, options) + "\n";
    if (report.empty())
    {
        return out;
    }

    const bool gpu = std::any_of(report.begin(), report.end(),
                                 [](const ServiceSample& sample) { return sample.gpuState == GpuState::Sampled; });
    std::vector<Column> columns{{"NAME", Align::Left},   {"STATE", Align::Left},  {"PID", Align::Right},
                                {"UPTIME", Align::Right}, {"CPU %", Align::Right}, {"MEMORY", Align::Right}};
    if (gpu)
    {
        columns.push_back(Column{"GPU %", Align::Right});
        columns.push_back(Column{"VRAM", Align::Right});
    }
    columns.push_back(Column{"HEARTBEAT", Align::Left});
    columns.push_back(Column{"RESTARTS", Align::Right});

    const Cell none{std::string{no_value}, {}};
    std::vector<std::vector<Cell>> rows{};
    for (const ServiceSample& sample : report)
    {
        const bool alive = IsAlive(sample.state) && sample.pid > 0;
        const std::optional<double> percent = CpuPercent(sample.name);
        std::vector<Cell> row{};
        row.push_back(Cell{sample.name, {}});
        row.push_back(Cell{std::string{StateName(sample.state)}, StateColor(sample.state)});
        row.push_back(alive ? Cell{std::to_string(sample.pid), {}} : none);
        row.push_back(alive && sample.startNs > 0
                          ? Cell{FormatDuration(std::chrono::nanoseconds{sample.snapshotNs - sample.startNs}), {}}
                          : none);
        row.push_back(alive && percent.has_value() ? Cell{OneDecimal(percent.value()), {}} : none);
        row.push_back(alive ? Cell{FormatBytes(sample.memoryBytes), {}} : none);
        if (gpu)
        {
            const bool sampled = alive && sample.gpuState == GpuState::Sampled;
            row.push_back(sampled && sample.gpuPercent >= 0 ? Cell{std::to_string(sample.gpuPercent), {}} : none);
            row.push_back(sampled ? Cell{FormatBytes(sample.vramBytes), {}} : none);
        }
        row.push_back(HeartbeatCell(sample));
        row.push_back(Cell{std::to_string(sample.restarts), {}});
        rows.push_back(row);
    }

    std::vector<std::size_t> widths{};
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        std::size_t width = columns[i].title.size();
        for (const std::vector<Cell>& row : rows)
        {
            width = std::max(width, row[i].text.size());
        }
        widths.push_back(width);
    }

    std::vector<std::vector<Cell>> lines{};
    std::vector<Cell> titles{};
    for (const Column& column : columns)
    {
        titles.push_back(Cell{std::string{column.title}, {}});
    }
    lines.push_back(titles);
    lines.insert(lines.end(), rows.begin(), rows.end());
    for (const std::vector<Cell>& line : lines)
    {
        std::string text{};
        for (std::size_t i = 0; i < columns.size(); ++i)
        {
            if (i > 0)
            {
                text.append(column_gap);
            }
            text.append(Padded(line[i], widths[i], columns[i].align, options.color));
        }
        // A left-aligned last cell would leave trailing spaces.
        while (!text.empty() && text.back() == ' ')
        {
            text.pop_back();
        }
        out += text + "\n";
    }
    return out;
}

std::optional<double> StatusTable::CpuPercent(std::string_view name) const
{
    const std::string key{name};
    if (!cpu.contains(key))
    {
        return std::nullopt;
    }

    return cpu.at(key);
}

const std::vector<ServiceSample>& StatusTable::Report() const
{
    return report;
}

} // namespace status_table
