#include <gtest/gtest.h>

#include "Logger.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{

constexpr int writer_threads = 4;
constexpr int lines_per_thread = 200;

// Points the console at a temporary file for one test, and puts the logger back afterwards.
class CapturedConsole
{
public:
    explicit CapturedConsole(bool journal)
        : stream{std::tmpfile()}
    {
        logger::Logger::Instance().Close();
        logger::Logger::Instance().SetMinimum(logger::Level::Verbose);
        logger::Logger::Instance().SetConsole(stream, journal);
    }

    ~CapturedConsole()
    {
        logger::Logger::Instance().SetConsole(nullptr, false);
        logger::Logger::Instance().Close();
        logger::Logger::Instance().SetMinimum(logger::Level::Debug);
        std::fclose(stream);
    }

    CapturedConsole(const CapturedConsole&) = delete;
    CapturedConsole& operator=(const CapturedConsole&) = delete;

    std::string Text() const
    {
        std::fflush(stream);
        std::rewind(stream);
        std::string text;
        char buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), stream)) > 0)
        {
            text.append(buffer, read);
        }

        return text;
    }

private:
    std::FILE* stream;
};

std::vector<std::string> Lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream input{text};
    std::string line;
    while (std::getline(input, line))
    {
        lines.push_back(line);
    }

    return lines;
}

std::filesystem::path FreshDirectory(const std::string& name)
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / (name + "_" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    return directory;
}

} // namespace

TEST(LoggerTest, FormatsTimeLevelAndText)
{
    const std::string line = logger::FormatLine(logger::Level::Info, std::chrono::system_clock::now(), "hello", false);
    ASSERT_EQ(line.size(), std::string{"2026-10-16 16:47:19.123 [I] hello\n"}.size());
    EXPECT_EQ(line.substr(23), " [I] hello\n");
    EXPECT_EQ(line[4], '-');
    EXPECT_EQ(line[19], '.');
}

TEST(LoggerTest, FormatsJournalPriorityInsteadOfTime)
{
    const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    EXPECT_EQ(logger::FormatLine(logger::Level::Error, now, "x", true), "<3>[E] x\n");
    EXPECT_EQ(logger::FormatLine(logger::Level::Warning, now, "x", true), "<4>[W] x\n");
    EXPECT_EQ(logger::FormatLine(logger::Level::Info, now, "x", true), "<6>[I] x\n");
    EXPECT_EQ(logger::FormatLine(logger::Level::Debug, now, "x", true), "<7>[D] x\n");
}

TEST(LoggerTest, FormatsPrintfArguments)
{
    const CapturedConsole console{true};
    logger::Info("[%s] pid %d exited with code %d", "A", 42, 3);
    EXPECT_EQ(console.Text(), "<6>[I] [A] pid 42 exited with code 3\n");
}

TEST(LoggerTest, DropsLinesBelowMinimum)
{
    const CapturedConsole console{true};
    logger::Logger::Instance().SetMinimum(logger::Level::Info);
    logger::Debug("hidden");
    logger::Warning("shown");
    EXPECT_FALSE(logger::Logger::Instance().Enabled(logger::Level::Debug));
    EXPECT_EQ(console.Text(), "<4>[W] shown\n");
}

TEST(LoggerTest, WritesLinesLongerThanTheStackBuffer)
{
    const CapturedConsole console{true};
    const std::string text(5000, 'x');
    logger::Error("%s", text.c_str());
    EXPECT_EQ(console.Text(), "<3>[E] " + text + "\n");
}

TEST(LoggerTest, WritesFileWithTimestampedLines)
{
    const CapturedConsole console{true};
    const std::filesystem::path directory = FreshDirectory("logger_file_test");
    logger::Settings settings{};
    settings.directory = directory;
    settings.prefix = "T";
    ASSERT_EQ(logger::Logger::Instance().Open(settings), logger::OpenCode::Ok);
    logger::Info("to the file");
    const std::filesystem::path path = logger::Logger::Instance().FilePath();
    ASSERT_FALSE(path.empty());
    EXPECT_EQ(path.parent_path(), directory);
    EXPECT_EQ(path.filename().string().rfind("T_", 0), 0U);
    std::ifstream file{path};
    std::string line;
    std::getline(file, line);
    EXPECT_EQ(line.substr(23), " [I] to the file");
    std::filesystem::remove_all(directory);
}

TEST(LoggerTest, StartsANewFileWhenFull)
{
    const CapturedConsole console{true};
    const std::filesystem::path directory = FreshDirectory("logger_rotate_test");
    logger::Settings settings{};
    settings.directory = directory;
    settings.prefix = "R";
    settings.maxFileBytes = 200;
    ASSERT_EQ(logger::Logger::Instance().Open(settings), logger::OpenCode::Ok);
    const std::filesystem::path first = logger::Logger::Instance().FilePath();
    for (int i = 0; i < 10; ++i)
    {
        logger::Info("line %d of a log that fills its files quickly", i);
    }

    const std::filesystem::path last = logger::Logger::Instance().FilePath();
    EXPECT_NE(first, last);
    int files = 0;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator{directory})
    {
        ++files;
        EXPECT_LE(entry.file_size(), 200U);
    }

    EXPECT_GE(files, 3);
    std::filesystem::remove_all(directory);
}

TEST(LoggerTest, DirectoryFailureKeepsTheConsole)
{
    const CapturedConsole console{true};
    logger::Settings settings{};
    settings.directory = "/proc/self/cannot_create_here";
    EXPECT_EQ(logger::Logger::Instance().Open(settings), logger::OpenCode::DirectoryFailed);
    logger::Info("still written");
    EXPECT_EQ(console.Text(), "<6>[I] still written\n");
}

TEST(LoggerTest, ConcurrentLinesStayWhole)
{
    const CapturedConsole console{true};
    std::vector<std::thread> writers;
    for (int t = 0; t < writer_threads; ++t)
    {
        writers.emplace_back(
            [t]()
            {
                for (int i = 0; i < lines_per_thread; ++i)
                {
                    logger::Info("writer %d line %d", t, i);
                }
            });
    }

    for (std::thread& writer : writers)
    {
        writer.join();
    }

    const std::vector<std::string> lines = Lines(console.Text());
    ASSERT_EQ(lines.size(), static_cast<std::size_t>(writer_threads * lines_per_thread));
    for (const std::string& line : lines)
    {
        EXPECT_EQ(line.rfind("<6>[I] writer ", 0), 0U) << line;
    }
}
