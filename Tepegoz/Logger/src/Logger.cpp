#include "Logger.hpp"

#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <syslog.h>
#include <system_error>
#include <unistd.h>

namespace logger
{

namespace
{

constexpr std::size_t stack_line_size = 1024;
constexpr int milliseconds_per_second = 1000;

char LevelLetter(Level level)
{
    switch (level)
    {
    case Level::Verbose:
        return 'V';
    case Level::Debug:
        return 'D';
    case Level::Info:
        return 'I';
    case Level::Warning:
        return 'W';
    case Level::Error:
        return 'E';
    }

    return '?';
}

int JournalPriority(Level level)
{
    switch (level)
    {
    case Level::Error:
        return LOG_ERR;
    case Level::Warning:
        return LOG_WARNING;
    case Level::Info:
        return LOG_INFO;
    case Level::Verbose:
    case Level::Debug:
        return LOG_DEBUG;
    }

    return LOG_INFO;
}

// systemd sets JOURNAL_STREAM to <device>:<inode> of the journal stream behind stdout and stderr.
bool IsJournal(std::FILE* stream)
{
    const char* value = std::getenv("JOURNAL_STREAM");
    unsigned long long device = 0;
    unsigned long long inode = 0;
    if (value == nullptr || std::sscanf(value, "%llu:%llu", &device, &inode) != 2)
    {
        return false;
    }

    struct stat status{};
    if (::fstat(::fileno(stream), &status) != 0)
    {
        return false;
    }

    return static_cast<unsigned long long>(status.st_dev) == device &&
           static_cast<unsigned long long>(status.st_ino) == inode;
}

// Colour only a terminal a person reads; https://no-color.org asks for none when NO_COLOR is set.
bool UseColor(std::FILE* stream, bool journal)
{
    const char* term = std::getenv("TERM");
    return !journal && std::getenv("NO_COLOR") == nullptr && ::isatty(::fileno(stream)) == 1 &&
           (term == nullptr || std::string_view{term} != "dumb");
}

std::tm LocalTime(std::chrono::system_clock::time_point when)
{
    const std::time_t seconds = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
    ::localtime_r(&seconds, &local);
    return local;
}

std::string StartStamp(std::chrono::system_clock::time_point when)
{
    const std::tm local = LocalTime(when);
    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y_%m_%d_%H_%M_%S", &local);
    return std::string{stamp};
}

void WriteFormatted(Level level, const char* format, std::va_list arguments)
{
    Logger& log = Logger::Instance();
    if (!log.Enabled(level))
    {
        return;
    }

    char buffer[stack_line_size] = {};
    std::va_list copy;
    va_copy(copy, arguments);
    const int length = std::vsnprintf(buffer, sizeof(buffer), format, copy);
    va_end(copy);
    if (length < 0)
    {
        return;
    }

    const std::size_t size = static_cast<std::size_t>(length);
    if (size < sizeof(buffer))
    {
        log.Write(level, std::string_view{buffer, size});
        return;
    }

    std::string text(size, '\0'); // a count and a fill character, not a list of two characters
    std::vsnprintf(text.data(), size + 1, format, arguments);
    log.Write(level, text);
}

} // namespace

std::string FormatLine(Level level, std::chrono::system_clock::time_point when, std::string_view text, bool journal)
{
    std::string line;
    if (journal)
    {
        line = "<" + std::to_string(JournalPriority(level)) + ">"; // journald keeps its own time
    }
    else
    {
        const std::tm local = LocalTime(when);
        const long long milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count() %
            milliseconds_per_second;
        char stamp[48] = {};
        const std::size_t length = std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
        std::snprintf(stamp + length, sizeof(stamp) - length, ".%03lld ", milliseconds);
        line = stamp;
    }

    line += '[';
    line += LevelLetter(level);
    line += "] ";
    line.append(text.data(), text.size());
    line += '\n';
    return line;
}

std::string ColorLine(Level level, std::string_view line)
{
    const char* start = nullptr;
    switch (level)
    {
    case Level::Error:
        start = "\033[1;31m"; // bold red
        break;
    case Level::Warning:
        start = "\033[33m"; // yellow
        break;
    case Level::Verbose:
    case Level::Debug:
        start = "\033[2m"; // dim
        break;
    case Level::Info:
        break;
    }
    if (start == nullptr)
    {
        return std::string{line};
    }

    const bool newline = !line.empty() && line.back() == '\n';
    std::string colored{start};
    colored.append(line.data(), newline ? line.size() - 1 : line.size());
    colored += "\033[0m";
    if (newline)
    {
        colored += '\n';
    }

    return colored;
}

void Logger::FileCloser::operator()(std::FILE* stream) const
{
    std::fclose(stream);
}

Logger& Logger::Instance()
{
    static Logger instance;
    return instance;
}

Logger::Logger()
    : journal{IsJournal(stderr)},
      color{UseColor(stderr, journal)}
{
}

OpenCode Logger::Open(const Settings& newSettings)
{
    const std::lock_guard<std::mutex> lock{mutex};
    settings = newSettings;
    minimum = newSettings.minimum;
    file.reset();
    filePath.clear();
    if (settings.directory.empty())
    {
        return OpenCode::Ok;
    }

    std::error_code failed;
    std::filesystem::create_directories(settings.directory, failed);
    if (failed)
    {
        return OpenCode::DirectoryFailed;
    }

    startTime = StartStamp(std::chrono::system_clock::now());
    fileIndex = 0;
    return StartFile() ? OpenCode::Ok : OpenCode::FileFailed;
}

void Logger::Close()
{
    const std::lock_guard<std::mutex> lock{mutex};
    file.reset();
    filePath.clear();
}

void Logger::SetMinimum(Level level)
{
    const std::lock_guard<std::mutex> lock{mutex};
    minimum = level;
}

bool Logger::Enabled(Level level) const
{
    const std::lock_guard<std::mutex> lock{mutex};
    return level >= minimum;
}

void Logger::SetConsole(std::FILE* stream, bool journalStream)
{
    const std::lock_guard<std::mutex> lock{mutex};
    console = stream != nullptr ? stream : stderr;
    journal = stream != nullptr ? journalStream : IsJournal(stderr);
    color = UseColor(console, journal);
}

void Logger::Write(Level level, std::string_view text)
{
    const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    const std::lock_guard<std::mutex> lock{mutex};
    if (level < minimum)
    {
        return;
    }

    const std::string consoleLine = FormatLine(level, now, text, journal);
    const std::string shown = color ? ColorLine(level, consoleLine) : consoleLine;
    std::fwrite(shown.data(), 1, shown.size(), console);
    std::fflush(console);
    if (!file)
    {
        return;
    }

    const std::string fileLine = journal ? FormatLine(level, now, text, false) : consoleLine;
    if (fileBytes > 0 && fileBytes + fileLine.size() > settings.maxFileBytes)
    {
        ++fileIndex;
        if (!StartFile())
        {
            return;
        }
    }

    std::fwrite(fileLine.data(), 1, fileLine.size(), file.get());
    std::fflush(file.get());
    fileBytes += fileLine.size();
}

std::filesystem::path Logger::FilePath() const
{
    const std::lock_guard<std::mutex> lock{mutex};
    return filePath;
}

bool Logger::StartFile()
{
    std::string name = settings.prefix + "_" + startTime;
    if (fileIndex > 0)
    {
        name += "_" + std::to_string(fileIndex);
    }

    filePath = settings.directory / (name + ".log");
    // "e": close-on-exec, so the services the manager starts do not inherit the log file.
    file.reset(std::fopen(filePath.c_str(), "ae"));
    fileBytes = 0;
    if (!file)
    {
        filePath.clear();
        return false;
    }

    std::error_code failed;
    const std::uintmax_t existing = std::filesystem::file_size(filePath, failed);
    if (!failed)
    {
        fileBytes = existing; // started twice within a second: appended to
    }

    return true;
}

void Verbose(const char* format, ...)
{
    std::va_list arguments;
    va_start(arguments, format);
    WriteFormatted(Level::Verbose, format, arguments);
    va_end(arguments);
}

void Debug(const char* format, ...)
{
    std::va_list arguments;
    va_start(arguments, format);
    WriteFormatted(Level::Debug, format, arguments);
    va_end(arguments);
}

void Info(const char* format, ...)
{
    std::va_list arguments;
    va_start(arguments, format);
    WriteFormatted(Level::Info, format, arguments);
    va_end(arguments);
}

void Warning(const char* format, ...)
{
    std::va_list arguments;
    va_start(arguments, format);
    WriteFormatted(Level::Warning, format, arguments);
    va_end(arguments);
}

void Error(const char* format, ...)
{
    std::va_list arguments;
    va_start(arguments, format);
    WriteFormatted(Level::Error, format, arguments);
    va_end(arguments);
}

} // namespace logger
