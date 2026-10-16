#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace logger
{

constexpr std::uint64_t default_max_file_bytes = 100ULL * 1024 * 1024;

enum class Level
{
    Verbose,
    Debug,
    Info,
    Warning,
    Error,
};

enum class OpenCode
{
    Ok,
    DirectoryFailed,
    FileFailed,
};

struct Settings
{
    Level minimum = Level::Debug;
    // Directory for the log files; empty writes to the console only.
    std::filesystem::path directory;
    // Files are named <prefix>_<start time>.log, then <prefix>_<start time>_<n>.log.
    std::string prefix = "PMLog";
    // A file that grows past this is closed, and the next one is started.
    std::uint64_t maxFileBytes = default_max_file_bytes;
};

/**
 * @brief Format one log line.
 * @param level Level of the line.
 * @param when Time the line was written.
 * @param text The message.
 * @param journal true for a console that is a journald stream: a priority prefix instead of the time.
 * @return The line, ending in a newline.
 */
std::string FormatLine(Level level, std::chrono::system_clock::time_point when, std::string_view text, bool journal);

// Writes log lines to stderr and, once opened, to rotating files. Every member is thread-safe.
class Logger
{
public:
    /**
     * @brief Return the process-wide logger. It writes to stderr until Open() adds files.
     * @return The logger.
     */
    static Logger& Instance();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /**
     * @brief Apply @p newSettings and start writing files in their directory.
     * @param newSettings Minimum level and where the files go.
     * @return OpenCode::Ok, otherwise why no file is written; the console keeps working either way.
     */
    OpenCode Open(const Settings& newSettings);

    /**
     * @brief Stop writing files; lines go to the console only.
     */
    void Close();

    /**
     * @brief Change the lowest level written.
     * @param level Lines below it are dropped.
     */
    void SetMinimum(Level level);

    /**
     * @brief Tell whether a line at @p level would be written.
     * @param level Level to check.
     * @return true when @p level is at or above the minimum.
     */
    bool Enabled(Level level) const;

    /**
     * @brief Send console lines to @p stream instead of stderr.
     * @param stream Stream to write to; it must outlive its use. nullptr returns to stderr.
     * @param journalStream Format console lines for journald; ignored for nullptr, where stderr is checked.
     */
    void SetConsole(std::FILE* stream, bool journalStream);

    /**
     * @brief Write one line at @p level to the console and the current file.
     * @param level Level of the line.
     * @param text The message, without a newline.
     */
    void Write(Level level, std::string_view text);

    /**
     * @brief Return the file being written.
     * @return Its path; empty when no file is open.
     */
    std::filesystem::path FilePath() const;

private:
    struct FileCloser
    {
        void operator()(std::FILE* file) const;
    };

    Logger();
    bool StartFile();

    mutable std::mutex mutex;
    Level minimum = Level::Debug;
    std::FILE* console = stderr;
    bool journal = false;
    Settings settings;
    std::string startTime;
    int fileIndex = 0;
    std::uint64_t fileBytes = 0;
    std::filesystem::path filePath;
    std::unique_ptr<std::FILE, FileCloser> file;
};

// printf-style: the format goes to vsnprintf, which needs a NUL-terminated string, so not std::string_view.
/**
 * @brief Write a printf-style line at Level::Verbose.
 * @param format printf format, followed by its arguments.
 */
void Verbose(const char* format, ...) __attribute__((format(printf, 1, 2)));

// printf-style: the format goes to vsnprintf, which needs a NUL-terminated string, so not std::string_view.
/**
 * @brief Write a printf-style line at Level::Debug.
 * @param format printf format, followed by its arguments.
 */
void Debug(const char* format, ...) __attribute__((format(printf, 1, 2)));

// printf-style: the format goes to vsnprintf, which needs a NUL-terminated string, so not std::string_view.
/**
 * @brief Write a printf-style line at Level::Info.
 * @param format printf format, followed by its arguments.
 */
void Info(const char* format, ...) __attribute__((format(printf, 1, 2)));

// printf-style: the format goes to vsnprintf, which needs a NUL-terminated string, so not std::string_view.
/**
 * @brief Write a printf-style line at Level::Warning.
 * @param format printf format, followed by its arguments.
 */
void Warning(const char* format, ...) __attribute__((format(printf, 1, 2)));

// printf-style: the format goes to vsnprintf, which needs a NUL-terminated string, so not std::string_view.
/**
 * @brief Write a printf-style line at Level::Error.
 * @param format printf format, followed by its arguments.
 */
void Error(const char* format, ...) __attribute__((format(printf, 1, 2)));

} // namespace logger
