#include "ServiceRegistry.hpp"

#include "ConfigSection.hpp"
#include "ConfigSource.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace service_registry
{

namespace
{

constexpr std::string_view name_key = "ProcessName";
constexpr std::string_view indent_step = "  ";
constexpr std::string_view temporary_suffix = ".tmp";
constexpr mode_t file_mode = 0644;

// The keys of a service in the order the generated template lists them; any other key follows in name order.
constexpr std::string_view key_order[] = {
    "ProcessName",   "ExecutablePath",           "Enabled",           "Args",
    "Cwd",           "WorkMode",                 "CreateProcessGroup", "InheritParentEnvironment",
    "StdioMode",     "StdioFilePath",            "StdioAppend",       "ForceKillTimeoutMs",
    "DependsOn",     "EnvironmentVariables",     "RestartMode",       "MaxRestarts",
    "RestartDelayMs", "BackoffMultiplier",       "RestartResetMs",    "CheckHealth",
    "GracePeriodMs", "HeartbeatPeriodMs",        "MissedHeartbeatThreshold", "UseDisplay",
    "Display",       "XAuthority",
};

struct OperationWord
{
    std::string_view word;
    Operation operation;
};

constexpr OperationWord operation_words[] = {
    {"version", Operation::Version}, {"list", Operation::List},       {"show", Operation::Show},
    {"add", Operation::Add},         {"replace", Operation::Replace}, {"update", Operation::Update},
    {"remove", Operation::Remove},
};

bool IsAsciiAlnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool IsScalar(const config_section::ConfigValue& value)
{
    return value.kind != config_section::ValueKind::Null && value.kind != config_section::ValueKind::Sequence &&
           value.kind != config_section::ValueKind::Mapping;
}

bool IsCollection(const config_section::ConfigValue& value)
{
    return value.kind == config_section::ValueKind::Sequence || value.kind == config_section::ValueKind::Mapping;
}

std::string Join(const std::vector<std::string_view>& words, std::string_view separator)
{
    std::string joined;
    for (const std::string_view word : words)
    {
        joined += (joined.empty() ? std::string{} : std::string{separator}) + std::string{word};
    }

    return joined;
}

std::vector<std::string_view> AllowedParameters(Operation operation)
{
    switch (operation)
    {
    case Operation::Show:
        return {"name"};
    case Operation::Add:
        return {"name", "definition", "start", "dryRun"};
    case Operation::Replace:
        return {"name", "definition", "restart", "dryRun"};
    case Operation::Update:
        return {"name", "set", "unset", "restart", "dryRun"};
    case Operation::Remove:
        return {"name", "force", "dryRun"};
    default:
        return {};
    }
}

bool NeedsName(Operation operation)
{
    return operation != Operation::Version && operation != Operation::List;
}

// problem is a second out-parameter: which parameter has the wrong kind of value.
RequestCode ReadFlag(const config_section::ConfigValue& value, std::string_view key, bool& out, std::string& problem)
{
    if (value.kind != config_section::ValueKind::Boolean)
    {
        problem = std::string{key} + " must be true or false";
        return RequestCode::WrongKind;
    }

    out = value.text == "true";
    return RequestCode::Ok;
}

// problem is a second out-parameter: which parameter has the wrong kind of value.
RequestCode ReadParameter(std::string_view key, const config_section::ConfigValue& value, Request& request,
                          std::string& problem)
{
    if (key == "name")
    {
        if (value.kind != config_section::ValueKind::String && value.kind != config_section::ValueKind::Integer)
        {
            problem = "name must be a text";
            return RequestCode::WrongKind;
        }

        request.name = value.text;
        return RequestCode::Ok;
    }

    if (key == "definition")
    {
        if (value.kind != config_section::ValueKind::Mapping)
        {
            problem = "definition must be a mapping of the service's keys to their values";
            return RequestCode::WrongKind;
        }

        request.definition = value;
        return RequestCode::Ok;
    }

    if (key == "set")
    {
        if (value.kind != config_section::ValueKind::Mapping && value.kind != config_section::ValueKind::Null)
        {
            problem = "set must be a mapping of keys to their new values";
            return RequestCode::WrongKind;
        }

        request.set = value;
        return RequestCode::Ok;
    }

    if (key == "unset")
    {
        if (value.kind == config_section::ValueKind::String)
        {
            request.unset.push_back(value.text);
            return RequestCode::Ok;
        }

        const bool listOfTexts =
            value.kind == config_section::ValueKind::Sequence &&
            std::all_of(value.children.begin(), value.children.end(),
                        [](const config_section::ConfigValue& item)
                        {
                            return item.kind == config_section::ValueKind::String;
                        });
        if (!listOfTexts && value.kind != config_section::ValueKind::Null)
        {
            problem = "unset must be a key, or a list of keys";
            return RequestCode::WrongKind;
        }

        for (const config_section::ConfigValue& item : value.children)
        {
            request.unset.push_back(item.text);
        }

        return RequestCode::Ok;
    }

    if (key == "start")
    {
        return ReadFlag(value, key, request.start, problem);
    }

    if (key == "dryRun")
    {
        return ReadFlag(value, key, request.dryRun, problem);
    }

    if (key == "restart")
    {
        return ReadFlag(value, key, request.restart, problem);
    }

    return ReadFlag(value, key, request.force, problem);
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

// Keeps the keys sorted, as the reader gives them.
void SetKey(config_section::ConfigValue& mapping, std::string_view key, const config_section::ConfigValue& value)
{
    const std::vector<std::string>::iterator at = std::lower_bound(mapping.keys.begin(), mapping.keys.end(), key);
    const std::size_t index = static_cast<std::size_t>(at - mapping.keys.begin());
    if (at != mapping.keys.end() && *at == key)
    {
        mapping.children[index] = value;
        return;
    }

    mapping.keys.insert(at, std::string{key});
    mapping.children.insert(mapping.children.begin() + static_cast<std::ptrdiff_t>(index), value);
}

void RemoveKey(config_section::ConfigValue& mapping, std::string_view key)
{
    const std::size_t index = KeyIndex(mapping, key);
    if (index == mapping.keys.size())
    {
        return;
    }

    mapping.keys.erase(mapping.keys.begin() + static_cast<std::ptrdiff_t>(index));
    mapping.children.erase(mapping.children.begin() + static_cast<std::ptrdiff_t>(index));
}

// The indexes of a mapping's keys: those of a service in the template's order first, then the others.
std::vector<std::size_t> OrderedKeys(const config_section::ConfigValue& mapping)
{
    std::vector<std::size_t> order;
    for (const std::string_view key : key_order)
    {
        const std::size_t index = KeyIndex(mapping, key);
        if (index < mapping.keys.size())
        {
            order.push_back(index);
        }
    }

    for (std::size_t i = 0; i < mapping.keys.size(); ++i)
    {
        if (std::find(order.begin(), order.end(), i) == order.end())
        {
            order.push_back(i);
        }
    }

    return order;
}

std::string DoubleQuoted(std::string_view text)
{
    std::string quoted{"\""};
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
        {
            quoted += '\\';
            quoted += c;
        }
        else if (c == '\n')
        {
            quoted += "\\n";
        }
        else if (c == '\t')
        {
            quoted += "\\t";
        }
        else if (c == '\r')
        {
            quoted += "\\r";
        }
        else if (byte < 0x20 || byte == 0x7f)
        {
            char escaped[8] = {};
            std::snprintf(escaped, sizeof(escaped), "\\x%02x", static_cast<unsigned int>(byte));
            quoted += escaped;
        }
        else
        {
            quoted += c;
        }
    }

    quoted += '"';
    return quoted;
}

// Characters a value may have and still be written without quotes; the reader then has the last word.
bool IsPlainCandidate(std::string_view text)
{
    constexpr std::string_view allowed = "_./-+=@%~:";
    if (text.empty() || text.back() == ':')
    {
        return false;
    }

    const char first = text.front();
    if (!IsAsciiAlnum(first) && first != '/' && first != '_')
    {
        return false;
    }

    return std::all_of(text.begin(), text.end(),
                       [allowed](char c)
                       {
                           return IsAsciiAlnum(c) || allowed.find(c) != std::string_view::npos;
                       });
}

// Whether the reader takes "key: <plain>" as the text itself, and not as a number, a boolean or anything else.
bool ReadsBackAsText(std::string_view plain, std::string_view text)
{
    config_section::ConfigValue parsed{};
    std::string error;
    if (config_section::ParseConfigText("value: " + std::string{plain} + "\n", parsed, error) !=
        config_section::LoadCode::Ok)
    {
        return false;
    }

    const config_section::ConfigValue* value = config_section::FindValue(parsed, "value");
    return value != nullptr && value->kind == config_section::ValueKind::String && value->text == text;
}

// Keys follow the rule of values: a key the reader would take for a number or a boolean sorts among the keys by
// that kind, so only a key that reads back as text is written without quotes.
std::string TextYaml(std::string_view text)
{
    return IsPlainCandidate(text) && ReadsBackAsText(text, text) ? std::string{text} : DoubleQuoted(text);
}

// A float as the reader gives it, written so that it reads back as a float: 2 becomes 2.0, inf .inf.
std::string FloatYaml(const std::string& text)
{
    if (text == "inf" || text == "+inf")
    {
        return ".inf";
    }

    if (text == "-inf")
    {
        return "-.inf";
    }

    if (text == "nan" || text == "-nan")
    {
        return ".nan";
    }

    const std::size_t exponent = text.find_first_of("eE");
    if (text.find('.') != std::string::npos)
    {
        return text;
    }

    if (exponent == std::string::npos)
    {
        return text + ".0";
    }

    return text.substr(0, exponent) + ".0" + text.substr(exponent);
}

std::string ScalarYaml(const config_section::ConfigValue& value)
{
    switch (value.kind)
    {
    case config_section::ValueKind::Null:
        return "null";
    case config_section::ValueKind::Float:
        return FloatYaml(value.text);
    case config_section::ValueKind::String:
        return TextYaml(value.text);
    default:
        return value.text; // a boolean or an integer, as the reader wrote it
    }
}

// For an item of a list that is itself a list or a mapping: written on one line, every text quoted.
std::string FlowYaml(const config_section::ConfigValue& value)
{
    if (value.kind == config_section::ValueKind::String)
    {
        return DoubleQuoted(value.text);
    }

    if (value.kind == config_section::ValueKind::Sequence)
    {
        std::string flow{"["};
        for (std::size_t i = 0; i < value.children.size(); ++i)
        {
            flow += (i == 0 ? "" : ", ") + FlowYaml(value.children[i]);
        }

        return flow + "]";
    }

    if (value.kind == config_section::ValueKind::Mapping)
    {
        std::string flow{"{"};
        const std::vector<std::size_t> order = OrderedKeys(value);
        for (std::size_t i = 0; i < order.size(); ++i)
        {
            flow += (i == 0 ? "" : ", ") + DoubleQuoted(value.keys[order[i]]) + ": " +
                    FlowYaml(value.children[order[i]]);
        }

        return flow + "}";
    }

    return ScalarYaml(value);
}

std::string Indent(std::size_t depth)
{
    std::string indent;
    for (std::size_t i = 0; i < depth; ++i)
    {
        indent += indent_step;
    }

    return indent;
}

void AppendEntry(std::string& out, std::size_t depth, std::string_view key, const config_section::ConfigValue& value)
{
    const std::string indent = Indent(depth);
    out += indent + TextYaml(key) + ":";
    if (value.kind == config_section::ValueKind::Sequence)
    {
        if (value.children.empty())
        {
            out += " []\n";
            return;
        }

        out += "\n";
        for (const config_section::ConfigValue& item : value.children)
        {
            out += indent + std::string{indent_step} + "- " + (IsCollection(item) ? FlowYaml(item) : ScalarYaml(item)) +
                   "\n";
        }

        return;
    }

    if (value.kind == config_section::ValueKind::Mapping)
    {
        if (value.keys.empty())
        {
            out += " {}\n";
            return;
        }

        out += "\n";
        for (const std::size_t index : OrderedKeys(value))
        {
            AppendEntry(out, depth + 1, value.keys[index], value.children[index]);
        }

        return;
    }

    out += " " + ScalarYaml(value) + "\n";
}

std::string JsonFloat(const std::string& text)
{
    if (text.find_first_of("in") != std::string::npos)
    {
        return JsonString(text); // inf and nan: JSON has no number for them
    }

    return text.find_first_of(".eE") == std::string::npos ? text + ".0" : text;
}

void SyncFolder(const std::filesystem::path& folder)
{
    const int descriptor = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor >= 0)
    {
        ::fsync(descriptor);
        ::close(descriptor);
    }
}

bool WriteAll(int descriptor, std::string_view text)
{
    while (!text.empty())
    {
        const ssize_t written = ::write(descriptor, text.data(), text.size());
        if (written < 0 && errno == EINTR)
        {
            continue;
        }

        if (written <= 0)
        {
            return false;
        }

        text.remove_prefix(static_cast<std::size_t>(written));
    }

    return true;
}

} // namespace

RequestCode ParseRequest(const std::vector<std::string>& frames, Request& out, std::string& problem)
{
    if (frames.empty() || frames[0].empty())
    {
        problem = "the request names no operation";
        return RequestCode::NoOperation;
    }

    std::size_t size = 0;
    for (const std::string& frame : frames)
    {
        size += frame.size();
    }

    if (size > max_request_bytes)
    {
        problem = "the request has " + std::to_string(size) + " bytes; a request has at most " +
                  std::to_string(max_request_bytes);
        return RequestCode::TooLarge;
    }

    if (frames.size() > 2)
    {
        problem = "a request has at most two frames: the operation and its parameters";
        return RequestCode::TooManyFrames;
    }

    Request request{};
    const OperationWord* known = nullptr;
    for (const OperationWord& word : operation_words)
    {
        if (word.word == frames[0])
        {
            known = &word;
        }
    }

    if (known == nullptr)
    {
        problem = "'" + frames[0] + "' is not an operation; the operations are version, list, show, add, replace, "
                  "update and remove";
        return RequestCode::UnknownOperation;
    }

    request.operation = known->operation;
    config_section::ConfigValue parameters{};
    if (frames.size() == 2)
    {
        std::string error;
        if (config_section::ParseConfigText(frames[1], parameters, error) != config_section::LoadCode::Ok)
        {
            problem = "the parameters are not valid YAML or JSON: " + error;
            return RequestCode::NotYaml;
        }
    }

    if (parameters.kind != config_section::ValueKind::Null && parameters.kind != config_section::ValueKind::Mapping)
    {
        problem = "the parameters must be a mapping of names to values";
        return RequestCode::NotMapping;
    }

    const std::vector<std::string_view> allowed = AllowedParameters(request.operation);
    for (std::size_t i = 0; i < parameters.keys.size(); ++i)
    {
        const std::string& key = parameters.keys[i];
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
        {
            problem = "'" + key + "' is not a parameter of " + std::string{OperationName(request.operation)} +
                      (allowed.empty() ? std::string{", which takes none"} : "; it takes " + Join(allowed, ", "));
            return RequestCode::UnknownParameter;
        }

        const RequestCode read = ReadParameter(key, parameters.children[i], request, problem);
        if (read != RequestCode::Ok)
        {
            return read;
        }
    }

    if (NeedsName(request.operation) && request.name.empty())
    {
        problem = std::string{OperationName(request.operation)} + " needs the name of a service";
        return RequestCode::MissingName;
    }

    const bool takesDefinition = request.operation == Operation::Add || request.operation == Operation::Replace;
    if (takesDefinition && request.definition.kind != config_section::ValueKind::Mapping)
    {
        problem = std::string{OperationName(request.operation)} + " needs a definition: the keys of the service";
        return RequestCode::MissingDefinition;
    }

    if (request.operation == Operation::Update && request.set.keys.empty() && request.unset.empty())
    {
        problem = "update needs keys to set or to unset";
        return RequestCode::MissingChange;
    }

    out = std::move(request);
    return RequestCode::Ok;
}

std::string_view OperationName(Operation operation)
{
    for (const OperationWord& word : operation_words)
    {
        if (word.operation == operation)
        {
            return word.word;
        }
    }

    return "version";
}

NameCode CheckName(std::string_view name)
{
    if (name.empty())
    {
        return NameCode::Empty;
    }

    if (name.size() > max_name_length)
    {
        return NameCode::TooLong;
    }

    if (name.front() == '.')
    {
        return NameCode::LeadingDot;
    }

    const bool allowed = std::all_of(name.begin(), name.end(),
                                     [](char c)
                                     {
                                         return IsAsciiAlnum(c) || c == '_' || c == '-' || c == '.';
                                     });
    return allowed ? NameCode::Ok : NameCode::BadCharacter;
}

std::string DescribeName(NameCode code, std::string_view name)
{
    const std::string quoted = "'" + std::string{name} + "'";
    switch (code)
    {
    case NameCode::Empty:
        return "the service needs a name";
    case NameCode::TooLong:
        return quoted + " has " + std::to_string(name.size()) + " bytes; a registered service's name has at most " +
               std::to_string(max_name_length);
    case NameCode::BadCharacter:
        return quoted + " is not a name for a registered service: use letters, digits, '_', '-' and '.' only";
    case NameCode::LeadingDot:
        return quoted + " is not a name for a registered service: it must not start with a dot";
    default:
        return std::string{};
    }
}

EditCode NameDefinition(config_section::ConfigValue& definition, std::string_view name)
{
    if (definition.kind != config_section::ValueKind::Mapping)
    {
        return EditCode::NotMapping;
    }

    const config_section::ConfigValue* stated = config_section::FindValue(definition, name_key);
    if (stated != nullptr && (!IsScalar(*stated) || stated->text != name))
    {
        return EditCode::NameDiffers;
    }

    config_section::ConfigValue named{};
    named.kind = config_section::ValueKind::String;
    named.text = std::string{name};
    SetKey(definition, name_key, named);
    return EditCode::Ok;
}

EditCode ApplyUpdate(config_section::ConfigValue& definition, const config_section::ConfigValue& set,
                     const std::vector<std::string>& unset)
{
    if (definition.kind != config_section::ValueKind::Mapping)
    {
        return EditCode::NotMapping;
    }

    if (set.kind != config_section::ValueKind::Mapping && set.kind != config_section::ValueKind::Null)
    {
        return EditCode::NotMapping;
    }

    const bool namesTheName = config_section::FindValue(set, name_key) != nullptr ||
                              std::find(unset.begin(), unset.end(), name_key) != unset.end();
    if (namesTheName)
    {
        return EditCode::NameNotEditable;
    }

    config_section::ConfigValue edited = definition;
    for (const std::string& key : unset)
    {
        RemoveKey(edited, key);
    }

    for (std::size_t i = 0; i < set.keys.size(); ++i)
    {
        SetKey(edited, set.keys[i], set.children[i]);
    }

    definition = std::move(edited);
    return EditCode::Ok;
}

std::string EmitYaml(const config_section::ConfigValue& definition)
{
    if (definition.kind != config_section::ValueKind::Mapping)
    {
        return FlowYaml(definition) + "\n";
    }

    std::string out;
    for (const std::size_t index : OrderedKeys(definition))
    {
        AppendEntry(out, 0, definition.keys[index], definition.children[index]);
    }

    return out;
}

std::string EmitJson(const config_section::ConfigValue& value)
{
    switch (value.kind)
    {
    case config_section::ValueKind::Null:
        return "null";
    case config_section::ValueKind::Boolean:
    case config_section::ValueKind::Integer:
        return value.text;
    case config_section::ValueKind::Float:
        return JsonFloat(value.text);
    case config_section::ValueKind::String:
        return JsonString(value.text);
    case config_section::ValueKind::Sequence:
    {
        std::string json{"["};
        for (std::size_t i = 0; i < value.children.size(); ++i)
        {
            json += (i == 0 ? "" : ",") + EmitJson(value.children[i]);
        }

        return json + "]";
    }
    case config_section::ValueKind::Mapping:
    {
        std::string json{"{"};
        const std::vector<std::size_t> order = OrderedKeys(value);
        for (std::size_t i = 0; i < order.size(); ++i)
        {
            json += (i == 0 ? "" : ",") + JsonString(value.keys[order[i]]) + ":" + EmitJson(value.children[order[i]]);
        }

        return json + "}";
    }
    }

    return "null";
}

std::string JsonString(std::string_view text)
{
    std::string json{"\""};
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
        {
            json += '\\';
            json += c;
        }
        else if (byte < 0x20)
        {
            char escaped[8] = {};
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned int>(byte));
            json += escaped;
        }
        else
        {
            json += c;
        }
    }

    json += '"';
    return json;
}

std::string_view StatusText(ReplyStatus status)
{
    switch (status)
    {
    case ReplyStatus::Ok:
        return "ok";
    case ReplyStatus::Refused:
        return "refused";
    default:
        return "error";
    }
}

std::vector<std::string> NewProblems(const std::vector<std::string>& before, const std::vector<std::string>& after)
{
    std::vector<std::string> remaining = before;
    std::vector<std::string> fresh;
    for (const std::string& problem : after)
    {
        const std::vector<std::string>::iterator found = std::find(remaining.begin(), remaining.end(), problem);
        if (found != remaining.end())
        {
            remaining.erase(found);
            continue;
        }

        fresh.push_back(problem);
    }

    return fresh;
}

void JsonObject::AddText(std::string_view key, std::string_view value)
{
    AddJson(key, JsonString(value));
}

void JsonObject::AddNumber(std::string_view key, long long value)
{
    AddJson(key, std::to_string(value));
}

void JsonObject::AddFlag(std::string_view key, bool value)
{
    AddJson(key, value ? "true" : "false");
}

void JsonObject::AddTexts(std::string_view key, const std::vector<std::string>& values)
{
    std::string json{"["};
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        json += (i == 0 ? "" : ",") + JsonString(values[i]);
    }

    AddJson(key, json + "]");
}

void JsonObject::AddJson(std::string_view key, std::string_view json)
{
    fields += (fields.empty() ? "" : ",") + JsonString(key) + ":" + std::string{json};
}

std::string JsonObject::Text() const
{
    return "{" + fields + "}";
}

ServiceFiles::ServiceFiles(std::filesystem::path folder)
    : folder{std::move(folder)}
{
}

std::filesystem::path ServiceFiles::FileOf(std::string_view name) const
{
    return folder / (std::string{name} + std::string{config_source::drop_in_extension});
}

bool ServiceFiles::Exists(std::string_view name) const
{
    std::error_code ignored{};
    // A folder that cannot be searched answers "none", which counts as there: nothing is written over it.
    return std::filesystem::symlink_status(FileOf(name), ignored).type() != std::filesystem::file_type::not_found;
}

StoreCode ServiceFiles::Read(std::string_view name, std::string& text) const
{
    const std::filesystem::path file = FileOf(name);
    std::error_code ignored{};
    if (!std::filesystem::is_regular_file(file, ignored))
    {
        return StoreCode::NotRead;
    }

    std::ifstream stream{file, std::ios::binary};
    if (!stream)
    {
        return StoreCode::NotRead;
    }

    std::ostringstream content;
    content << stream.rdbuf();
    text = content.str();
    return StoreCode::Ok;
}

StoreCode ServiceFiles::Save(std::string_view name, Snapshot& out) const
{
    Snapshot snapshot{};
    snapshot.name = std::string{name};
    std::error_code ignored{};
    snapshot.folderExisted = std::filesystem::is_directory(folder, ignored);
    if (Exists(name))
    {
        snapshot.fileExisted = true;
        if (Read(name, snapshot.text) != StoreCode::Ok)
        {
            return StoreCode::NotRead;
        }
    }

    out = std::move(snapshot);
    return StoreCode::Ok;
}

StoreCode ServiceFiles::Write(std::string_view name, std::string_view text) const
{
    std::error_code error{};
    std::filesystem::create_directories(folder, error);
    if (error)
    {
        return StoreCode::FolderNotMade;
    }

    const std::filesystem::path target = FileOf(name);
    // A dot file: the loader ignores it, so a reload in between never reads half a file.
    const std::filesystem::path temporary =
        folder / ("." + target.filename().string() + std::string{temporary_suffix});
    const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, file_mode);
    if (descriptor < 0)
    {
        return StoreCode::NotWritten;
    }

    const bool written = WriteAll(descriptor, text) && ::fsync(descriptor) == 0;
    const bool closed = ::close(descriptor) == 0;
    if (!written || !closed || ::rename(temporary.c_str(), target.c_str()) != 0)
    {
        ::unlink(temporary.c_str());
        return StoreCode::NotWritten;
    }

    SyncFolder(folder);
    return StoreCode::Ok;
}

StoreCode ServiceFiles::Remove(std::string_view name) const
{
    std::error_code error{};
    std::filesystem::remove(FileOf(name), error);
    if (error)
    {
        return StoreCode::NotRemoved;
    }

    SyncFolder(folder);
    return StoreCode::Ok;
}

StoreCode ServiceFiles::Restore(const Snapshot& snapshot) const
{
    if (snapshot.fileExisted)
    {
        return Write(snapshot.name, snapshot.text);
    }

    const StoreCode removed = Remove(snapshot.name);
    if (removed != StoreCode::Ok)
    {
        return removed;
    }

    if (!snapshot.folderExisted)
    {
        std::error_code ignored{};
        std::filesystem::remove(folder, ignored); // only when it is empty again
    }

    return StoreCode::Ok;
}

const std::filesystem::path& ServiceFiles::Folder() const
{
    return folder;
}

} // namespace service_registry
