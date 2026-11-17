#include <gtest/gtest.h>

#include "StatusTable.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace
{

// 2023-11-14 22:13:20 UTC.
constexpr std::int64_t snapshot_ns = 1700000000LL * 1000000000LL;
constexpr std::int64_t second_ns = 1000000000LL;
constexpr std::uint64_t mib = 1024 * 1024;

status_table::ServiceSample Sample(const std::string& name, status_table::ServiceState state, int pid)
{
    status_table::ServiceSample sample{};
    sample.name = name;
    sample.state = state;
    sample.pid = pid;
    sample.memoryBytes = 100 * mib;
    sample.startNs = snapshot_ns - 90 * second_ns;
    sample.lastSeenNs = sample.startNs;
    sample.snapshotNs = snapshot_ns;
    return sample;
}

std::vector<std::string> Lines(const std::string& text)
{
    std::vector<std::string> lines{};
    std::istringstream in{text};
    std::string line{};
    while (std::getline(in, line))
    {
        lines.push_back(line);
    }
    return lines;
}

std::string WithoutColor(const std::string& text)
{
    return std::regex_replace(text, std::regex{"\033\\[[0-9;]*m"}, "");
}

// The row of the service named first on it.
std::string RowOf(const std::string& table, const std::string& name)
{
    for (const std::string& line : Lines(table))
    {
        if (line.rfind(name + " ", 0) == 0)
        {
            return line;
        }
    }
    return {};
}

} // namespace

TEST(StatusTableTest, FormatsShortDurationInSeconds)
{
    EXPECT_EQ(status_table::FormatDuration(std::chrono::milliseconds{45200}), "45.2s");
}

TEST(StatusTableTest, FormatsMinutesAndHours)
{
    EXPECT_EQ(status_table::FormatDuration(std::chrono::seconds{31 * 60 + 51}), "31:51");
    EXPECT_EQ(status_table::FormatDuration(std::chrono::seconds{2 * 3600 + 14 * 60 + 5}), "02:14:05");
}

TEST(StatusTableTest, NegativeDurationHasNoValue)
{
    EXPECT_EQ(status_table::FormatDuration(std::chrono::seconds{-1}), "-");
}

TEST(StatusTableTest, FormatsBytesInTheLargestUnit)
{
    EXPECT_EQ(status_table::FormatBytes(0), "0 B");
    EXPECT_EQ(status_table::FormatBytes(512), "512 B");
    EXPECT_EQ(status_table::FormatBytes(224499302), "214.1 MB");
    EXPECT_EQ(status_table::FormatBytes(1536 * mib), "1.5 GB");
}

TEST(StatusTableTest, CpuNeedsTwoReportsOfTheSameProcess)
{
    status_table::StatusTable table{};
    status_table::ServiceSample sample = Sample("server", status_table::ServiceState::Running, 10);
    sample.cpuUsec = 1000000;
    table.Update({sample});
    EXPECT_FALSE(table.CpuPercent("server").has_value());

    sample.cpuUsec += 500000;
    sample.snapshotNs += second_ns;
    table.Update({sample});
    const std::optional<double> percent = table.CpuPercent("server");
    ASSERT_TRUE(percent.has_value());
    EXPECT_DOUBLE_EQ(percent.value(), 50.0);
}

TEST(StatusTableTest, CpuStartsAgainForANewProcess)
{
    status_table::StatusTable table{};
    status_table::ServiceSample sample = Sample("server", status_table::ServiceState::Running, 10);
    table.Update({sample});
    sample.cpuUsec += 500000;
    sample.snapshotNs += second_ns;
    table.Update({sample});
    ASSERT_TRUE(table.CpuPercent("server").has_value());

    sample.pid = 11;
    sample.snapshotNs += second_ns;
    table.Update({sample});
    EXPECT_FALSE(table.CpuPercent("server").has_value());
}

TEST(StatusTableTest, CpuKeepsItsFigureForReportsCloseTogether)
{
    status_table::StatusTable table{};
    status_table::ServiceSample sample = Sample("server", status_table::ServiceState::Running, 10);
    table.Update({sample});
    sample.cpuUsec += 500000;
    sample.snapshotNs += second_ns;
    table.Update({sample});

    // A report sent at once for a state change, 100 ms later, with the CPU busy all of it.
    sample.cpuUsec += 100000;
    sample.snapshotNs += second_ns / 10;
    table.Update({sample});
    const std::optional<double> percent = table.CpuPercent("server");
    ASSERT_TRUE(percent.has_value());
    EXPECT_DOUBLE_EQ(percent.value(), 50.0);
}

TEST(StatusTableTest, ServiceThatLeftTheReportsStartsItsCpuAgain)
{
    status_table::StatusTable table{};
    status_table::ServiceSample sample = Sample("server", status_table::ServiceState::Running, 10);
    table.Update({sample});
    sample.cpuUsec += 500000;
    sample.snapshotNs += second_ns;
    table.Update({sample});
    table.Update({});

    sample.cpuUsec += 500000;
    sample.snapshotNs += second_ns;
    table.Update({sample});
    EXPECT_FALSE(table.CpuPercent("server").has_value());
}

TEST(StatusTableTest, ColumnsFitTheLongestName)
{
    status_table::StatusTable table{};
    table.Update({Sample("a", status_table::ServiceState::Running, 10),
                  Sample("telemetry_bridge", status_table::ServiceState::Running, 11)});
    const std::string text = table.Render(status_table::TableOptions{});
    const std::vector<std::string> lines = Lines(text);
    ASSERT_EQ(lines.size(), 4u);
    const std::size_t column = lines[1].find("STATE");
    EXPECT_GT(column, std::string{"telemetry_bridge"}.size());
    EXPECT_EQ(lines[2].find("Running"), column);
    EXPECT_EQ(lines[3].find("Running"), column);
}

TEST(StatusTableTest, UnhealthyServiceShowsCpuAndMemory)
{
    status_table::StatusTable table{};
    status_table::ServiceSample sample = Sample("server", status_table::ServiceState::Unhealthy, 10);
    sample.missedBeats = 3;
    table.Update({sample});
    sample.cpuUsec += 250000;
    sample.snapshotNs += second_ns;
    table.Update({sample});
    const std::string row = RowOf(table.Render(status_table::TableOptions{}), "server");
    EXPECT_NE(row.find("25.0"), std::string::npos);
    EXPECT_NE(row.find("100.0 MB"), std::string::npos);
    EXPECT_NE(row.find("3 missed"), std::string::npos);
}

TEST(StatusTableTest, StoppedServiceShowsNoProcessFigures)
{
    status_table::StatusTable table{};
    status_table::ServiceSample sample = Sample("server", status_table::ServiceState::Stopped, -1);
    sample.restarts = 5;
    table.Update({sample});
    const std::string row = RowOf(table.Render(status_table::TableOptions{}), "server");
    EXPECT_EQ(row.find("MB"), std::string::npos);
    EXPECT_EQ(row.find("-1"), std::string::npos);
    EXPECT_NE(row.find("Stopped"), std::string::npos);
    EXPECT_EQ(row.back(), '5');
}

TEST(StatusTableTest, HeaderCountsServicesByState)
{
    ::setenv("TZ", "UTC", 1);
    ::tzset();
    status_table::StatusTable table{};
    table.Update({Sample("a", status_table::ServiceState::Running, 10),
                  Sample("b", status_table::ServiceState::Running, 11),
                  Sample("c", status_table::ServiceState::Unhealthy, 12),
                  Sample("d", status_table::ServiceState::Stopped, -1)});
    status_table::TableOptions options{};
    options.machine = "target-01";
    const std::string header = Lines(table.Render(options)).front();
    EXPECT_EQ(header, "target-01 | 4 services: 2 running, 1 unhealthy, 1 stopped | GPU not sampled yet | 22:13:20");
}

TEST(StatusTableTest, NoticeEndsTheHeader)
{
    status_table::StatusTable table{};
    table.Update({Sample("a", status_table::ServiceState::Running, 10)});
    status_table::TableOptions options{};
    options.notice = "No report for 7s";
    const std::string header = Lines(table.Render(options)).front();
    EXPECT_EQ(header.substr(header.size() - std::string{" | No report for 7s"}.size()), " | No report for 7s");
}

TEST(StatusTableTest, NoServicesSaysSo)
{
    status_table::StatusTable table{};
    table.Update({});
    status_table::TableOptions options{};
    options.machine = "target-01";
    EXPECT_EQ(table.Render(options), "target-01 | no services\n");
}

TEST(StatusTableTest, GpuColumnsOnlyWithGpuData)
{
    status_table::StatusTable table{};
    status_table::ServiceSample sample = Sample("vision", status_table::ServiceState::Running, 10);
    table.Update({sample});
    EXPECT_EQ(table.Render(status_table::TableOptions{}).find("VRAM"), std::string::npos);

    sample.gpuState = status_table::GpuState::Sampled;
    sample.gpuPercent = 37;
    sample.vramBytes = 512 * mib;
    table.Update({sample});
    const std::string text = table.Render(status_table::TableOptions{});
    EXPECT_NE(text.find("VRAM"), std::string::npos);
    EXPECT_NE(RowOf(text, "vision").find("512.0 MB"), std::string::npos);
    EXPECT_NE(Lines(text).front().find("GPU ok"), std::string::npos);
}

TEST(StatusTableTest, HeartbeatShowsHowLongAgo)
{
    status_table::StatusTable table{};
    status_table::ServiceSample sample = Sample("server", status_table::ServiceState::Running, 10);
    sample.lastSeenNs = snapshot_ns - 300 * 1000000LL;
    table.Update({sample});
    EXPECT_NE(RowOf(table.Render(status_table::TableOptions{}), "server").find("0.3s ago"), std::string::npos);
}

TEST(StatusTableTest, ServiceWithoutHeartbeatsShowsNone)
{
    status_table::StatusTable table{};
    table.Update({Sample("server", status_table::ServiceState::Running, 10)});
    EXPECT_EQ(RowOf(table.Render(status_table::TableOptions{}), "server").find("ago"), std::string::npos);
}

TEST(StatusTableTest, ColorsKeepTheColumnsAligned)
{
    status_table::StatusTable table{};
    table.Update({Sample("a", status_table::ServiceState::Running, 10),
                  Sample("bb", status_table::ServiceState::Unhealthy, 11),
                  Sample("ccc", status_table::ServiceState::Stopped, -1)});
    status_table::TableOptions plain{};
    status_table::TableOptions colored{};
    colored.color = true;
    const std::string text = table.Render(colored);
    EXPECT_NE(text.find("\033["), std::string::npos);
    EXPECT_EQ(WithoutColor(text), table.Render(plain));
}

TEST(StatusTableTest, PlainTableHasNoEscapes)
{
    status_table::StatusTable table{};
    table.Update({Sample("a", status_table::ServiceState::Unhealthy, 10)});
    status_table::TableOptions options{};
    options.notice = "No report for 7s";
    EXPECT_EQ(table.Render(options).find('\033'), std::string::npos);
}

TEST(StatusTableTest, MarkedServiceShowsTheMarkAfterItsName)
{
    status_table::StatusTable table{};
    status_table::ServiceSample sample = Sample("server", status_table::ServiceState::Running, 10);
    sample.restartToApply = true;
    table.Update({sample, Sample("other", status_table::ServiceState::Running, 11)});
    const std::string text = table.Render(status_table::TableOptions{});
    EXPECT_EQ(RowOf(text, "server").rfind("server *", 0), 0u);
    EXPECT_EQ(RowOf(text, "other").rfind("other ", 0), 0u);
    EXPECT_EQ(RowOf(text, "other").find('*'), std::string::npos);
}

TEST(StatusTableTest, MarkIsExplainedUnderTheTable)
{
    status_table::StatusTable table{};
    status_table::ServiceSample sample = Sample("server", status_table::ServiceState::Stopped, -1);
    sample.restartToApply = true;
    table.Update({sample});
    const std::vector<std::string> lines = Lines(table.Render(status_table::TableOptions{}));
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_EQ(lines.back(), std::string{status_table::restart_note});
}

TEST(StatusTableTest, TableWithoutAMarkHasNoNote)
{
    status_table::StatusTable table{};
    table.Update({Sample("server", status_table::ServiceState::Running, 10)});
    const std::string text = table.Render(status_table::TableOptions{});
    EXPECT_EQ(Lines(text).size(), 3u);
    EXPECT_EQ(text.find("restart to apply"), std::string::npos);
    EXPECT_EQ(text.find('*'), std::string::npos);
}
