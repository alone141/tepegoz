#include "RegistryClient.hpp"

#include "ConfigSection.hpp"
#include "ServiceRegistry.hpp"
#include "StatusTable.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <zmq.hpp>
#include <zmq_addon.hpp>

namespace registry_client
{

namespace
{

struct VerbWord
{
    std::string_view word;
    Verb verb;
};

// An option that a verb either takes or refuses, and whether the command line gave it.
struct OptionUse
{
    std::string_view name;
    bool given;
    bool taken;
};

constexpr VerbWord verb_words[] = {
    {"add", Verb::Add},       {"set", Verb::Set},   {"replace", Verb::Replace},
    {"remove", Verb::Remove}, {"show", Verb::Show}, {"list", Verb::List},
};

// The long options of the whole command line that take a value, so the word after one is not the command.
constexpr std::string_view long_value_options[] = {"start", "disable", "kill", "restart", "config",
                                                   "from",  "set",     "unset", "registry"};
// Their short spellings: -s, -d, -k, -r and -c.
constexpr std::string_view short_value_options = "sdkrc";
constexpr std::string_view blanks = " \t\r\n";
// Besides a letter or a digit, the characters a number, a boolean or null can start with.
constexpr std::string_view typed_starts = "+-.~";
constexpr std::string_view executable_key = "ExecutablePath";
constexpr std::string_view args_key = "Args";
constexpr std::string_view list_header[] = {"NAME", "ORIGIN", "STATE", "PID", "MARK", "FILE"};
constexpr std::size_t pid_column = 3;
constexpr std::string_view column_gap = "  ";
constexpr std::string_view problem_mark = "!";
constexpr std::string_view restart_mark = "*";

bool IsAsciiAlnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool TakesValue(std::string_view longName)
{
    return std::find(std::begin(long_value_options), std::end(long_value_options), longName) !=
           std::end(long_value_options);
}

// A group of short options such as -s or -hc: the last one that takes a value takes the next word, unless more
// letters follow it, which are then its value.
bool ShortTakesNext(std::string_view group)
{
    for (std::size_t i = 0; i < group.size(); ++i)
    {
        if (short_value_options.find(group[i]) != std::string_view::npos)
        {
            return i + 1 == group.size();
        }
    }

    return false;
}

config_section::ConfigValue Text(std::string_view text)
{
    config_section::ConfigValue value{};
    value.kind = config_section::ValueKind::String;
    value.text = std::string{text};
    return value;
}

config_section::ConfigValue EmptyMapping()
{
    config_section::ConfigValue mapping{};
    mapping.kind = config_section::ValueKind::Mapping;
    return mapping;
}

std::size_t KeyIndex(const config_section::ConfigValue& mapping, std::string_view key)
{
    for (std::size_t i = 0; i < mapping.keys.size(); ++i)
    {
        if (mapping.keys[i] == key)
        {
            return i;
        }
    }

    return mapping.keys.size();
}

bool HasKey(const config_section::ConfigValue& mapping, std::string_view key)
{
    return KeyIndex(mapping, key) < mapping.keys.size();
}

void PutKey(config_section::ConfigValue& mapping, std::string_view key, const config_section::ConfigValue& value)
{
    const std::size_t index = KeyIndex(mapping, key);
    if (index < mapping.keys.size())
    {
        mapping.children[index] = value;
        return;
    }

    mapping.keys.push_back(std::string{key});
    mapping.children.push_back(value);
}

bool IsTyped(config_section::ValueKind kind)
{
    return kind == config_section::ValueKind::Integer || kind == config_section::ValueKind::Float ||
           kind == config_section::ValueKind::Boolean || kind == config_section::ValueKind::Null;
}

// Only a single word that starts like a number, a boolean or null is offered to the reader as one; anything else
// is a text, so that a comment, a colon or an indicator in it is never taken for YAML.
bool MayBeTyped(std::string_view text)
{
    const char first = text.front();
    const bool startsLikeOne = IsAsciiAlnum(first) || typed_starts.find(first) != std::string_view::npos;
    return startsLikeOne && text.find_first_of(blanks) == std::string_view::npos &&
           text.find('#') == std::string_view::npos;
}

// The reader takes "a" b for the text b, so a quoted value is checked first: its opening quote must close at its
// last character, past a doubled '' or a \" escape.
bool ClosesAtTheEnd(std::string_view text)
{
    const char quote = text.front();
    std::size_t i = 1;
    while (i < text.size())
    {
        if (quote == '"' && text[i] == '\\')
        {
            i += 2;
            continue;
        }

        if (text[i] == quote)
        {
            if (quote == '\'' && i + 1 < text.size() && text[i + 1] == '\'')
            {
                i += 2;
                continue;
            }

            return i + 1 == text.size();
        }

        ++i;
    }

    return false;
}

std::string Quoted(std::string_view text)
{
    return "'" + std::string{text} + "'";
}

std::string Usage(Verb verb)
{
    switch (verb)
    {
    case Verb::Add:
        return "add NAME EXECUTABLE [ARG...], or add NAME --from FILE";
    case Verb::Set:
        return "set NAME Key=Value... [--unset Key]";
    case Verb::Replace:
        return "replace NAME --from FILE";
    case Verb::Remove:
        return "remove NAME";
    case Verb::Show:
        return "show NAME";
    default:
        return "list";
    }
}

// problem is a second out-parameter: the option the verb does not take.
BuildCode CheckOptions(const VerbLine& line, std::string& problem)
{
    const Verb verb = line.verb;
    const bool changes = verb == Verb::Add || verb == Verb::Set || verb == Verb::Replace || verb == Verb::Remove;
    const OptionUse uses[] = {
        {"--from", !line.from.empty(), verb == Verb::Add || verb == Verb::Replace},
        {"--set", !line.sets.empty(), verb == Verb::Add || verb == Verb::Set},
        {"--unset", !line.unsets.empty(), verb == Verb::Set},
        {"--no-start", line.noStart, verb == Verb::Add},
        {"--restart", line.restart, verb == Verb::Set || verb == Verb::Replace},
        {"--force", line.force, verb == Verb::Remove},
        {"--dry-run", line.dryRun, changes},
        {"--json", line.json, verb == Verb::Show || verb == Verb::List},
    };
    for (const OptionUse& use : uses)
    {
        if (use.given && !use.taken)
        {
            problem = std::string{VerbName(verb)} + " does not take " + std::string{use.name};
            return BuildCode::BadOption;
        }
    }

    return BuildCode::Ok;
}

// key and value are two out-parameters, and problem a third: why the word cannot be read.
BuildCode ReadAssignment(std::string_view word, Verb verb, std::string& key, config_section::ConfigValue& value,
                         std::string& problem)
{
    const std::size_t equals = word.find('=');
    if (equals == std::string_view::npos)
    {
        problem = Quoted(word) + " is not Key=Value, such as MaxRestarts=3";
        return BuildCode::BadWords;
    }

    if (equals == 0)
    {
        problem = Quoted(word) + " has no key before '='";
        return BuildCode::BadWords;
    }

    const std::string name{word.substr(0, equals)};
    std::string why;
    const ValueCode read = ReadValue(word.substr(equals + 1), value, why);
    if (read == ValueCode::Empty)
    {
        problem = Quoted(name + "=") + " has no value: give one, or write '" + name + "=\"\"' for an empty text" +
                  (verb == Verb::Set ? ", or remove the key with --unset " + name : std::string{});
        return BuildCode::BadValue;
    }

    if (read != ValueCode::Ok)
    {
        problem = "the value of " + name + " cannot be read: " + why;
        return BuildCode::BadValue;
    }

    key = name;
    return BuildCode::Ok;
}

// problem is a second out-parameter: the word that cannot be read, or the key given twice.
BuildCode ReadAssignments(const std::vector<std::string>& words, Verb verb, config_section::ConfigValue& mapping,
                          std::string& problem)
{
    for (const std::string& word : words)
    {
        std::string key;
        config_section::ConfigValue value{};
        const BuildCode read = ReadAssignment(word, verb, key, value, problem);
        if (read != BuildCode::Ok)
        {
            return read;
        }

        if (HasKey(mapping, key))
        {
            problem = key + " is given twice";
            return BuildCode::BadWords;
        }

        PutKey(mapping, key, value);
    }

    return BuildCode::Ok;
}

// problem is a second out-parameter: why the file does not give a definition.
BuildCode ReadDefinitionFile(std::string_view from, const std::filesystem::path& folder,
                             config_section::ConfigValue& out, std::string& problem)
{
    const std::filesystem::path given{std::string{from}};
    const std::filesystem::path file = given.is_absolute() ? given : folder / given;
    std::string error;
    const config_section::LoadCode loaded = config_section::LoadConfigFile(file, out, error);
    if (loaded == config_section::LoadCode::FileNotOpened)
    {
        problem = error;
        return BuildCode::BadFile;
    }

    if (loaded != config_section::LoadCode::Ok)
    {
        problem = file.string() + " is not valid YAML: " + error;
        return BuildCode::BadFile;
    }

    if (out.kind != config_section::ValueKind::Mapping)
    {
        problem = file.string() + " does not hold the keys of one service, such as ExecutablePath: /opt/app/bin/server";
        return BuildCode::BadFile;
    }

    return BuildCode::Ok;
}

// problem is a second out-parameter: what is wrong with the words of add.
BuildCode AddDefinition(const VerbLine& line, const std::filesystem::path& folder,
                        config_section::ConfigValue& definition, std::string& problem)
{
    definition = EmptyMapping();
    bool argsFromWords = false;
    if (!line.from.empty())
    {
        if (line.words.size() > 1)
        {
            problem = "add NAME --from FILE takes no EXECUTABLE: the file gives ExecutablePath";
            return BuildCode::BadWords;
        }

        const BuildCode read = ReadDefinitionFile(line.from, folder, definition, problem);
        if (read != BuildCode::Ok)
        {
            return read;
        }
    }
    else
    {
        if (line.words.size() < 2)
        {
            problem = "add needs a NAME and the program to run: " + Usage(Verb::Add);
            return BuildCode::BadWords;
        }

        PutKey(definition, executable_key, Text(AbsoluteProgram(line.words[1], folder)));
        if (line.words.size() > 2)
        {
            config_section::ConfigValue args{};
            args.kind = config_section::ValueKind::Sequence;
            for (std::size_t i = 2; i < line.words.size(); ++i)
            {
                args.children.push_back(Text(line.words[i]));
            }

            PutKey(definition, args_key, args);
            argsFromWords = true;
        }
    }

    config_section::ConfigValue extra = EmptyMapping();
    const BuildCode read = ReadAssignments(line.sets, Verb::Add, extra, problem);
    if (read != BuildCode::Ok)
    {
        return read;
    }

    for (std::size_t i = 0; i < extra.keys.size(); ++i)
    {
        const std::string& key = extra.keys[i];
        if (line.from.empty() && key == executable_key)
        {
            problem = "ExecutablePath is given twice: as EXECUTABLE and with --set";
            return BuildCode::BadWords;
        }

        if (argsFromWords && key == args_key)
        {
            problem = "Args is given twice: as the words after EXECUTABLE and with --set";
            return BuildCode::BadWords;
        }

        PutKey(definition, key, extra.children[i]);
    }

    return BuildCode::Ok;
}

// problem is a second out-parameter: what is wrong with the words of set.
BuildCode SetParameters(const VerbLine& line, service_registry::JsonObject& parameters, std::string& problem)
{
    std::vector<std::string> assignments(line.words.begin() + 1, line.words.end());
    assignments.insert(assignments.end(), line.sets.begin(), line.sets.end());
    config_section::ConfigValue changes = EmptyMapping();
    const BuildCode read = ReadAssignments(assignments, Verb::Set, changes, problem);
    if (read != BuildCode::Ok)
    {
        return read;
    }

    for (const std::string& key : line.unsets)
    {
        if (key.empty())
        {
            problem = "--unset needs the key to remove";
            return BuildCode::BadWords;
        }

        if (HasKey(changes, key))
        {
            problem = key + " is both set and unset";
            return BuildCode::BadWords;
        }
    }

    if (changes.keys.empty() && line.unsets.empty())
    {
        problem = "set needs keys to change: " + Usage(Verb::Set);
        return BuildCode::BadWords;
    }

    if (!changes.keys.empty())
    {
        parameters.AddJson("set", service_registry::EmitJson(changes));
    }

    if (!line.unsets.empty())
    {
        parameters.AddTexts("unset", line.unsets);
    }

    return BuildCode::Ok;
}

std::string TextOf(const config_section::ConfigValue& mapping, std::string_view key)
{
    const config_section::ConfigValue* value = config_section::FindValue(mapping, key);
    if (value == nullptr || value->kind == config_section::ValueKind::Null ||
        value->kind == config_section::ValueKind::Sequence || value->kind == config_section::ValueKind::Mapping)
    {
        return std::string{};
    }

    return value->text;
}

bool FlagOf(const config_section::ConfigValue& mapping, std::string_view key)
{
    const config_section::ConfigValue* value = config_section::FindValue(mapping, key);
    return value != nullptr && value->kind == config_section::ValueKind::Boolean && value->text == "true";
}

long long NumberOf(const config_section::ConfigValue& mapping, std::string_view key, long long fallback)
{
    const config_section::ConfigValue* value = config_section::FindValue(mapping, key);
    if (value == nullptr || value->kind != config_section::ValueKind::Integer)
    {
        return fallback;
    }

    const std::string& text = value->text;
    long long number = fallback;
    const std::from_chars_result read = std::from_chars(text.data(), text.data() + text.size(), number);
    return read.ec == std::errc{} ? number : fallback;
}

std::vector<std::string> TextsOf(const config_section::ConfigValue& mapping, std::string_view key)
{
    std::vector<std::string> texts;
    const config_section::ConfigValue* value = config_section::FindValue(mapping, key);
    if (value == nullptr || value->kind != config_section::ValueKind::Sequence)
    {
        return texts;
    }

    for (const config_section::ConfigValue& item : value->children)
    {
        texts.push_back(item.text);
    }

    return texts;
}

std::string Capitalized(std::string text)
{
    if (!text.empty() && text.front() >= 'a' && text.front() <= 'z')
    {
        text.front() = static_cast<char>(text.front() - 'a' + 'A');
    }

    return text;
}

// A service the manager does not have is Disabled, by Enabled: false or --disable, or Skipped for its file.
std::string StateWord(const config_section::ConfigValue& service)
{
    if (FlagOf(service, "loaded"))
    {
        return TextOf(service, "state");
    }

    return TextOf(service, "problem").empty() ? "Disabled" : "Skipped";
}

std::string StateLine(const config_section::ConfigValue& service)
{
    std::string line = "State: " + StateWord(service);
    const long long pid = NumberOf(service, "pid", -1);
    if (pid > 0)
    {
        line += ", pid " + std::to_string(pid);
    }

    const std::string workMode = TextOf(service, "workMode");
    if (FlagOf(service, "loaded") && !workMode.empty())
    {
        line += ", work mode " + workMode;
    }

    if (FlagOf(service, "restartToApply"))
    {
        line += "; restart to apply: its new definition takes effect at its next start";
    }

    return line;
}

std::string Padded(std::string_view text, std::size_t width, bool right)
{
    const std::string fill(width > text.size() ? width - text.size() : 0, ' ');
    return right ? fill + std::string{text} : std::string{text} + fill;
}

std::string ListTable(const config_section::ConfigValue& reply)
{
    std::vector<std::vector<std::string>> rows{std::vector<std::string>(std::begin(list_header), std::end(list_header))};
    std::vector<std::string> problems;
    bool marked = false;
    const config_section::ConfigValue* services = config_section::FindValue(reply, "services");
    if (services != nullptr && services->kind == config_section::ValueKind::Sequence)
    {
        for (const config_section::ConfigValue& service : services->children)
        {
            const long long pid = NumberOf(service, "pid", -1);
            const std::string problem = TextOf(service, "problem");
            const bool waiting = FlagOf(service, "restartToApply");
            std::string mark = waiting ? std::string{restart_mark} : std::string{};
            if (!problem.empty())
            {
                mark += problem_mark;
                problems.push_back(problem);
            }

            marked = marked || waiting;
            rows.push_back({TextOf(service, "name"), TextOf(service, "origin"), StateWord(service),
                            pid > 0 ? std::to_string(pid) : "-", mark, TextOf(service, "file")});
        }
    }

    std::vector<std::size_t> widths(std::size(list_header), 0);
    for (const std::vector<std::string>& row : rows)
    {
        for (std::size_t column = 0; column < row.size(); ++column)
        {
            widths[column] = std::max(widths[column], row[column].size());
        }
    }

    std::string table;
    for (const std::vector<std::string>& row : rows)
    {
        std::string line;
        for (std::size_t column = 0; column < row.size(); ++column)
        {
            const bool last = column + 1 == row.size();
            line += (column == 0 ? std::string{} : std::string{column_gap}) +
                    (last ? row[column] : Padded(row[column], widths[column], column == pid_column));
        }

        table += line.substr(0, line.find_last_not_of(' ') + 1) + "\n";
    }

    if (marked)
    {
        table += std::string{status_table::restart_note} + "\n";
    }

    for (const std::string& problem : problems)
    {
        table += std::string{problem_mark} + " " + problem + "\n";
    }

    return table;
}

void RenderShow(const VerbLine& line, const config_section::ConfigValue& reply, Printed& printed)
{
    const config_section::ConfigValue* definition = config_section::FindValue(reply, "definition");
    const bool skipped = definition == nullptr || definition->kind != config_section::ValueKind::Mapping;
    const std::string name = line.words.empty() ? std::string{"NAME"} : line.words.front();
    if (skipped)
    {
        printed.out = TextOf(reply, "fileText");
        printed.err = TextOf(reply, "problem") + "\n" + "The file is skipped. To repair it: show " + name +
                      " > FILE, edit FILE, then replace " + name + " --from FILE\n";
    }
    else
    {
        printed.out = TextOf(reply, "yaml");
        printed.err = Capitalized(TextOf(reply, "message")) + "\n";
    }

    printed.err += "File: " + TextOf(reply, "file") + "\n" + StateLine(reply) + "\n";
}

void RenderChange(const VerbLine& line, const config_section::ConfigValue& reply, Printed& printed)
{
    const std::string facts = Capitalized(TextOf(reply, "message")) + "\n" + "File: " + TextOf(reply, "file") + "\n";
    if (line.dryRun && line.verb != Verb::Remove)
    {
        printed.out = TextOf(reply, "yaml");
        printed.err = facts;
        return;
    }

    printed.out = facts;
}

std::optional<Outcome> OutcomeOf(std::string_view status)
{
    if (status == service_registry::StatusText(service_registry::ReplyStatus::Ok))
    {
        return Outcome::Done;
    }

    if (status == service_registry::StatusText(service_registry::ReplyStatus::Refused))
    {
        return Outcome::Refused;
    }

    if (status == service_registry::StatusText(service_registry::ReplyStatus::Error))
    {
        return Outcome::NotUnderstood;
    }

    return std::nullopt;
}

std::string DurationText(std::chrono::milliseconds duration)
{
    if (duration.count() % 1000 == 0)
    {
        return std::to_string(duration.count() / 1000) + "s";
    }

    return std::to_string(duration.count()) + " ms";
}

} // namespace

std::optional<Verb> VerbOf(std::string_view word)
{
    for (const VerbWord& known : verb_words)
    {
        if (known.word == word)
        {
            return known.verb;
        }
    }

    return std::nullopt;
}

std::string_view VerbName(Verb verb)
{
    for (const VerbWord& known : verb_words)
    {
        if (known.verb == verb)
        {
            return known.word;
        }
    }

    return "list";
}

std::optional<Verb> FindVerb(std::span<const std::string> args)
{
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string_view arg = args[i];
        if (arg == "--")
        {
            if (i + 1 < args.size())
            {
                return VerbOf(args[i + 1]);
            }

            return std::nullopt;
        }

        if (arg.starts_with("--"))
        {
            if (arg.find('=') == std::string_view::npos && TakesValue(arg.substr(2)))
            {
                ++i;
            }

            continue;
        }

        if (arg.size() > 1 && arg.front() == '-')
        {
            if (ShortTakesNext(arg.substr(1)))
            {
                ++i;
            }

            continue;
        }

        return VerbOf(arg);
    }

    return std::nullopt;
}

ValueCode ReadValue(std::string_view text, config_section::ConfigValue& out, std::string& problem)
{
    if (text.empty())
    {
        problem = "the value is empty";
        return ValueCode::Empty;
    }

    const char first = text.front();
    config_section::ConfigValue parsed{};
    std::string error;
    if (first == '"' || first == '\'')
    {
        if (!ClosesAtTheEnd(text))
        {
            error = "its closing quote must be its last character";
        }

        const bool read = error.empty() &&
                          config_section::ParseConfigText(text, parsed, error) == config_section::LoadCode::Ok;
        if (!read || parsed.kind != config_section::ValueKind::String)
        {
            problem = "a value that starts with a quote is read as a quoted YAML text, and " + std::string{text} +
                      " is not one" + (error.empty() ? std::string{} : ": " + error);
            return ValueCode::BadQuotes;
        }

        out = parsed;
        return ValueCode::Ok;
    }

    if (first == '[' || first == '{')
    {
        const char closing = first == '[' ? ']' : '}';
        if (text.back() != closing)
        {
            error = "it does not end with " + std::string(1, closing);
        }

        const bool read = error.empty() &&
                          config_section::ParseConfigText(text, parsed, error) == config_section::LoadCode::Ok;
        if (!read ||
            (parsed.kind != config_section::ValueKind::Sequence && parsed.kind != config_section::ValueKind::Mapping))
        {
            problem = "a value that starts with [ or { is read as a YAML list or mapping, and " + std::string{text} +
                      " is not one" + (error.empty() ? std::string{} : ": " + error);
            return ValueCode::BadCollection;
        }

        out = parsed;
        return ValueCode::Ok;
    }

    if (MayBeTyped(text) && config_section::ParseConfigText(text, parsed, error) == config_section::LoadCode::Ok &&
        IsTyped(parsed.kind))
    {
        out = parsed;
        return ValueCode::Ok;
    }

    out = Text(text);
    return ValueCode::Ok;
}

std::string AbsoluteProgram(std::string_view executable, const std::filesystem::path& folder)
{
    const std::filesystem::path program{std::string{executable}};
    if (executable.find('/') == std::string_view::npos || program.is_absolute())
    {
        return std::string{executable};
    }

    return (folder / program).lexically_normal().string();
}

std::string RegistryEndpoint(std::string_view given)
{
    if (given.empty())
    {
        return std::string{default_registry_endpoint};
    }

    if (given.find("://") != std::string_view::npos)
    {
        return std::string{given};
    }

    // A port follows the host, which an IPv6 address puts in brackets: [::1]:6671.
    const std::size_t bracket = given.rfind(']');
    const std::string_view afterHost = bracket == std::string_view::npos ? given : given.substr(bracket);
    const bool hasPort = afterHost.find(':') != std::string_view::npos;
    return "tcp://" + std::string{given} + (hasPort ? std::string{} : ":" + std::string{default_registry_port});
}

BuildCode BuildRequest(const VerbLine& line, const std::filesystem::path& folder, std::vector<std::string>& frames,
                       std::string& problem)
{
    const BuildCode options = CheckOptions(line, problem);
    if (options != BuildCode::Ok)
    {
        return options;
    }

    const std::string verb{VerbName(line.verb)};
    if (line.verb == Verb::List)
    {
        if (!line.words.empty())
        {
            problem = "list takes no words: it lists every service";
            return BuildCode::BadWords;
        }

        frames = {"list"};
        return BuildCode::Ok;
    }

    if (line.words.empty() || line.words.front().empty())
    {
        problem = verb + " needs the name of a service: " + Usage(line.verb);
        return BuildCode::BadWords;
    }

    const bool oneWord = line.verb == Verb::Replace || line.verb == Verb::Remove || line.verb == Verb::Show;
    if (oneWord && line.words.size() > 1)
    {
        problem = verb + " takes one NAME, and " + Quoted(line.words[1]) + " is one more: " + Usage(line.verb);
        return BuildCode::BadWords;
    }

    service_registry::JsonObject parameters{};
    parameters.AddText("name", line.words.front());
    std::string operation = verb;
    if (line.verb == Verb::Add)
    {
        config_section::ConfigValue definition{};
        const BuildCode built = AddDefinition(line, folder, definition, problem);
        if (built != BuildCode::Ok)
        {
            return built;
        }

        parameters.AddJson("definition", service_registry::EmitJson(definition));
        if (line.noStart)
        {
            parameters.AddFlag("start", false);
        }
    }
    else if (line.verb == Verb::Replace)
    {
        if (line.from.empty())
        {
            problem = "replace needs the new definition in a file: " + Usage(Verb::Replace);
            return BuildCode::BadWords;
        }

        config_section::ConfigValue definition{};
        const BuildCode read = ReadDefinitionFile(line.from, folder, definition, problem);
        if (read != BuildCode::Ok)
        {
            return read;
        }

        parameters.AddJson("definition", service_registry::EmitJson(definition));
    }
    else if (line.verb == Verb::Set)
    {
        operation = std::string{service_registry::OperationName(service_registry::Operation::Update)};
        const BuildCode built = SetParameters(line, parameters, problem);
        if (built != BuildCode::Ok)
        {
            return built;
        }
    }

    if (line.restart)
    {
        parameters.AddFlag("restart", true);
    }

    if (line.force)
    {
        parameters.AddFlag("force", true);
    }

    if (line.dryRun)
    {
        parameters.AddFlag("dryRun", true);
    }

    frames = {operation, parameters.Text()};
    return BuildCode::Ok;
}

SendCode SendRequest(std::string_view endpoint, const std::vector<std::string>& frames,
                     std::chrono::milliseconds timeout, Answer& out)
{
    zmq::context_t context{1};
    zmq::socket_t socket{context, zmq::socket_type::dealer};
    socket.set(zmq::sockopt::linger, 0);
    // Give up when the manager is not running instead of waiting for it forever.
    socket.set(zmq::sockopt::immediate, 1);
    socket.set(zmq::sockopt::sndtimeo, static_cast<int>(send_timeout.count()));
    try
    {
        socket.connect(std::string{endpoint});
    }
    catch (const zmq::error_t&)
    {
        return SendCode::BadEndpoint;
    }

    std::vector<zmq::message_t> reply;
    try
    {
        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            const zmq::send_flags flags = i + 1 < frames.size() ? zmq::send_flags::sndmore : zmq::send_flags::none;
            if (!socket.send(zmq::buffer(frames[i]), flags).has_value())
            {
                return SendCode::NotReachable;
            }
        }

        zmq::pollitem_t items[] = {{socket.handle(), 0, ZMQ_POLLIN, 0}};
        zmq::poll(items, 1, timeout);
        if ((items[0].revents & ZMQ_POLLIN) == 0)
        {
            return SendCode::TimedOut;
        }

        if (!zmq::recv_multipart(socket, std::back_inserter(reply), zmq::recv_flags::dontwait).has_value())
        {
            return SendCode::BadAnswer;
        }
    }
    catch (const zmq::error_t&)
    {
        return SendCode::NotReachable;
    }

    if (reply.size() != 2)
    {
        return SendCode::BadAnswer;
    }

    out = Answer{reply[0].to_string(), reply[1].to_string()};
    return SendCode::Answered;
}

Printed RenderReply(const VerbLine& line, const Answer& answer)
{
    Printed printed{};
    const std::optional<Outcome> outcome = OutcomeOf(answer.status);
    if (!outcome.has_value())
    {
        printed.err = "The manager's answer could not be read: its status is " + Quoted(answer.status) +
                      ", not ok, refused or error\n";
        printed.outcome = Outcome::BadAnswer;
        return printed;
    }

    printed.outcome = *outcome;
    if (line.json)
    {
        printed.out = answer.json + "\n";
        return printed;
    }

    config_section::ConfigValue reply{};
    std::string error;
    if (config_section::ParseConfigText(answer.json, reply, error) != config_section::LoadCode::Ok ||
        reply.kind != config_section::ValueKind::Mapping)
    {
        printed.err = "The manager's answer could not be read" + (error.empty() ? std::string{} : ": " + error) + "\n";
        printed.outcome = Outcome::BadAnswer;
        return printed;
    }

    const std::string message = TextOf(reply, "message");
    if (*outcome == Outcome::NotUnderstood)
    {
        printed.err = "The manager did not understand the request: " + message + "\n";
        return printed;
    }

    if (*outcome == Outcome::Refused)
    {
        printed.err = "Refused: " + message + "\n";
        for (const std::string& problem : TextsOf(reply, "problems"))
        {
            printed.err += "  " + problem + "\n";
        }

        return printed;
    }

    if (line.verb == Verb::List)
    {
        printed.out = ListTable(reply);
    }
    else if (line.verb == Verb::Show)
    {
        RenderShow(line, reply, printed);
    }
    else
    {
        RenderChange(line, reply, printed);
    }

    return printed;
}

Outcome RunVerb(const VerbLine& line, const std::filesystem::path& folder, std::chrono::milliseconds timeout)
{
    std::vector<std::string> frames;
    std::string problem;
    if (BuildRequest(line, folder, frames, problem) != BuildCode::Ok)
    {
        // A problem that starts with a command keeps it as it is typed: "add needs ...".
        const bool startsWithVerb = VerbOf(std::string_view{problem}.substr(0, problem.find(' '))).has_value();
        std::cerr << (startsWithVerb ? problem : Capitalized(problem)) << ". Use --help to list the commands.\n";
        return Outcome::BadCommandLine;
    }

    const std::string endpoint = RegistryEndpoint(line.registry);
    Answer answer{};
    const SendCode sent = SendRequest(endpoint, frames, timeout, answer);
    if (sent == SendCode::BadEndpoint)
    {
        std::cerr << Quoted(line.registry) << " is not an endpoint: give HOST, HOST:PORT or tcp://HOST:PORT\n";
        return Outcome::BadCommandLine;
    }

    if (sent == SendCode::NotReachable)
    {
        std::cerr << "The process manager's service registry is not reachable at " << endpoint
                  << (endpoint == default_registry_endpoint
                          ? std::string{}
                          : "; a manager on another machine takes requests only with RemoteRegistration: true")
                  << "\n";
        return Outcome::NotReachable;
    }

    if (sent == SendCode::TimedOut)
    {
        std::cerr << "No answer from the process manager within " << DurationText(timeout)
                  << "; its log tells whether the request was done\n";
        return Outcome::TimedOut;
    }

    if (sent == SendCode::BadAnswer)
    {
        std::cerr << "The answer from " << endpoint << " is not a service registry's answer\n";
        return Outcome::BadAnswer;
    }

    const Printed printed = RenderReply(line, answer);
    std::cerr << printed.err << std::flush;
    std::cout << printed.out << std::flush;
    return printed.outcome;
}

int ExitCode(Outcome outcome)
{
    if (outcome == Outcome::Done)
    {
        return 0;
    }

    return outcome == Outcome::TimedOut ? 2 : 1;
}

} // namespace registry_client
