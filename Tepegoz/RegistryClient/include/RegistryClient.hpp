#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace config_section
{
struct ConfigValue;
} // namespace config_section

namespace registry_client
{

constexpr std::string_view default_registry_endpoint = "tcp://localhost:6671";
constexpr std::string_view default_registry_port = "6671";
// How long a request waits for the manager's answer, as reload does.
constexpr std::chrono::milliseconds reply_timeout{10000};
// How long a request waits to be sent before the manager counts as not reachable, as for the other commands.
constexpr std::chrono::milliseconds send_timeout{2000};

enum class Verb
{
    Add,
    Set,
    Replace,
    Remove,
    Show,
    List,
};

// One registry verb as the command line gave it: the words after the verb, and its options.
struct VerbLine
{
    Verb verb = Verb::List;
    // add: NAME EXECUTABLE ARG...; set: NAME Key=Value...; replace, remove and show: NAME; list: none.
    std::vector<std::string> words;
    // --from FILE, relative to the current folder; empty when not given.
    std::string from;
    // --set Key=Value, in the order given.
    std::vector<std::string> sets;
    // --unset Key, in the order given.
    std::vector<std::string> unsets;
    bool noStart = false;
    bool restart = false;
    bool force = false;
    bool dryRun = false;
    bool json = false;
    // --registry: HOST, HOST:PORT or an endpoint; empty for default_registry_endpoint.
    std::string registry;
};

enum class ValueCode
{
    Ok,
    Empty,
    BadQuotes,
    BadCollection,
};

enum class BuildCode
{
    Ok,
    BadWords,
    BadOption,
    BadValue,
    BadFile,
};

enum class SendCode
{
    Answered,
    BadEndpoint,
    NotReachable,
    TimedOut,
    BadAnswer,
};

enum class Outcome
{
    Done,
    Refused,
    NotUnderstood,
    BadAnswer,
    BadCommandLine,
    NotReachable,
    TimedOut,
};

// The manager's answer: the status word of its first frame and the JSON object of its second.
struct Answer
{
    std::string status;
    std::string json;
};

// What a verb prints, and how it ends.
struct Printed
{
    // For standard output.
    std::string out;
    // For standard error.
    std::string err;
    Outcome outcome = Outcome::Done;
};

/**
 * @brief Find the verb a word names.
 * @param word A word of the command line, such as "add".
 * @return The verb, or std::nullopt when the word names none.
 */
std::optional<Verb> VerbOf(std::string_view word);

/**
 * @brief Return the word that names a verb.
 * @param verb The verb.
 * @return "add", "set", "replace", "remove", "show" or "list".
 */
std::string_view VerbName(Verb verb);

/**
 * @brief Find the command of a command line and tell whether it is a registry verb.
 * @param args The arguments after the program's name. The values of the options that take one are skipped.
 * @return The verb when the command is one, otherwise std::nullopt.
 */
std::optional<Verb> FindVerb(std::span<const std::string> args);

// problem is a second out-parameter: why the value cannot be read, worded for whoever typed it.
/**
 * @brief Read the value of a Key=Value word.
 * @param text The text after the first '='. A quoted text, a list in [] and a mapping in {} are read as YAML;
 *             a number, true, false or null as that; anything else as the text exactly as typed.
 * @param out Receives the value on success.
 * @param problem Receives why the value cannot be read.
 * @return ValueCode::Ok, Empty, BadQuotes or BadCollection.
 */
ValueCode ReadValue(std::string_view text, config_section::ConfigValue& out, std::string& problem);

/**
 * @brief Make a relative path of a program absolute; leave an absolute path and a bare program name alone.
 * @param executable The program as typed.
 * @param folder The current folder of the command line.
 * @return @p executable joined to @p folder when it has a '/' and is relative, otherwise @p executable.
 */
std::string AbsoluteProgram(std::string_view executable, const std::filesystem::path& folder);

/**
 * @brief Turn the value of --registry into an endpoint.
 * @param given HOST, HOST:PORT, an endpoint such as tcp://HOST:PORT, or empty.
 * @return The endpoint: tcp:// and the default port are added when missing; empty gives the default endpoint.
 */
std::string RegistryEndpoint(std::string_view given);

// problem is a second out-parameter: what is wrong with the command line, worded for whoever typed it.
/**
 * @brief Build the request a verb sends: the operation and its parameters as JSON.
 * @param line The verb, its words and its options.
 * @param folder The current folder, for a relative EXECUTABLE and --from FILE.
 * @param frames Receives the request's frames on success.
 * @param problem Receives what is wrong with the command line.
 * @return BuildCode::Ok, otherwise the failure.
 */
BuildCode BuildRequest(const VerbLine& line, const std::filesystem::path& folder, std::vector<std::string>& frames,
                       std::string& problem);

/**
 * @brief Send one request to the service registry and wait for its answer.
 * @param endpoint Where the registry listens.
 * @param frames The request's frames.
 * @param timeout How long the answer is waited for once the request is sent.
 * @param out Receives the answer on success.
 * @return SendCode::Answered, otherwise the failure.
 */
SendCode SendRequest(std::string_view endpoint, const std::vector<std::string>& frames,
                     std::chrono::milliseconds timeout, Answer& out);

/**
 * @brief Word the manager's answer for a person, and choose the outcome.
 * @param line The verb that was sent, with its options.
 * @param answer The manager's answer.
 * @return What goes to standard output and standard error, and the outcome.
 */
Printed RenderReply(const VerbLine& line, const Answer& answer);

/**
 * @brief Run a verb: build its request, send it, wait for the answer and print it.
 * @param line The verb, its words and its options.
 * @param folder The current folder, for a relative EXECUTABLE and --from FILE.
 * @param timeout How long the answer is waited for.
 * @return The outcome.
 */
Outcome RunVerb(const VerbLine& line, const std::filesystem::path& folder, std::chrono::milliseconds timeout);

/**
 * @brief Turn a verb's outcome into the command line's exit code.
 * @param outcome The outcome.
 * @return 0 for Done, 2 for TimedOut, otherwise 1.
 */
int ExitCode(Outcome outcome);

} // namespace registry_client
