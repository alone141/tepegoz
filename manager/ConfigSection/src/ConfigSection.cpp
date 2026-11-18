#include "ConfigSection.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fkYAML/node.hpp>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace config_section
{

namespace
{

constexpr std::string_view blanks = " \t";
constexpr int max_float_digits = 17;

std::string_view Trim(std::string_view text)
{
    const std::size_t first = text.find_first_not_of(blanks);
    if (first == std::string_view::npos)
    {
        return std::string_view{};
    }

    const std::size_t last = text.find_last_not_of(blanks);
    return text.substr(first, last - first + 1);
}

bool EqualsIgnoringCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }

    return true;
}

// The shortest text that reads back as the same number, so 0.1 stays "0.1".
std::string ShortestText(double number)
{
    char buffer[32] = {};
    for (int digits = 1; digits <= max_float_digits; ++digits)
    {
        std::snprintf(buffer, sizeof(buffer), "%.*g", digits, number);
        if (std::strtod(buffer, nullptr) == number)
        {
            break;
        }
    }

    return std::string{buffer};
}

std::string IntegerText(const fkyaml::node& node)
{
    try
    {
        return std::to_string(node.as_int());
    }
    catch (const fkyaml::type_error&)
    {
        return std::to_string(node.as_uint()); // above INT64_MAX
    }
}

std::string_view LineOf(std::string_view text, unsigned int index)
{
    for (unsigned int line = 0; line < index; ++line)
    {
        const std::size_t end = text.find('\n');
        if (end == std::string_view::npos)
        {
            return std::string_view{};
        }

        text.remove_prefix(end + 1);
    }

    text = text.substr(0, text.find('\n'));
    if (!text.empty() && text.back() == '\r')
    {
        text.remove_suffix(1);
    }

    return text;
}

// fkYAML 0.4.4 cannot read two spaces in a row inside a value without quotes ("a  b"), which YAML
// allows, and reports a misleading error at or after that line. Names the line and the fix.
std::string DoubleSpaceHint(std::string_view text, unsigned int line)
{
    for (unsigned int index : {line == 0 ? 0 : line - 1, line})
    {
        std::string_view value = LineOf(text, index);
        value = value.substr(0, value.find(" #"));
        const std::size_t colon = value.find(": ");
        value = Trim(colon == std::string_view::npos ? value : value.substr(colon + 2));
        if (value.substr(0, 2) == "- ")
        {
            value = Trim(value.substr(2));
        }

        if (!value.empty() && value.front() != '"' && value.front() != '\'' &&
            value.find("  ") != std::string_view::npos)
        {
            return ". Line " + std::to_string(index + 1) +
                   " has two spaces in a row in a value without quotes, which this parser cannot read: put "
                   "that value in quotes";
        }
    }

    return std::string{};
}

// fkYAML counts lines and columns from 0; editors count them from 1.
std::string DescribeParseError(std::string_view what, std::string_view text)
{
    constexpr std::string_view kind = "parse_error: ";
    constexpr std::string_view where = " (at line ";
    if (what.substr(0, kind.size()) == kind)
    {
        what.remove_prefix(kind.size());
    }

    const std::size_t at = what.rfind(where);
    unsigned int line = 0;
    unsigned int column = 0;
    if (at != std::string_view::npos &&
        std::sscanf(std::string{what.substr(at)}.c_str(), " (at line %u, column %u)", &line, &column) == 2)
    {
        std::string_view message = what.substr(0, at);
        if (!message.empty() && message.back() == '.')
        {
            message.remove_suffix(1);
        }

        return "line " + std::to_string(line + 1) + ", column " + std::to_string(column + 1) + ": " +
               std::string{message} + DoubleSpaceHint(text, line);
    }

    return std::string{what};
}

bool Convert(const fkyaml::node& node, ConfigValue& out, std::string& error)
{
    switch (node.get_type())
    {
    case fkyaml::node_type::NULL_OBJECT:
        out.kind = ValueKind::Null;
        return true;
    case fkyaml::node_type::BOOLEAN:
        out.kind = ValueKind::Boolean;
        out.text = node.as_bool() ? "true" : "false";
        return true;
    case fkyaml::node_type::INTEGER:
        out.kind = ValueKind::Integer;
        out.text = IntegerText(node);
        return true;
    case fkyaml::node_type::FLOAT:
        out.kind = ValueKind::Float;
        out.text = ShortestText(node.as_float());
        return true;
    case fkyaml::node_type::STRING:
        out.kind = ValueKind::String;
        out.text = node.as_str();
        return true;
    case fkyaml::node_type::SEQUENCE:
        out.kind = ValueKind::Sequence;
        for (const fkyaml::node& item : node)
        {
            ConfigValue child{};
            if (!Convert(item, child, error))
            {
                return false;
            }

            out.children.push_back(std::move(child));
        }

        return true;
    case fkyaml::node_type::MAPPING:
        out.kind = ValueKind::Mapping;
        for (fkyaml::node::const_iterator entry = node.begin(); entry != node.end(); ++entry)
        {
            ConfigValue key{};
            if (!entry.key().is_scalar() || !Convert(entry.key(), key, error))
            {
                error = "a mapping key must be a single value";
                return false;
            }

            ConfigValue child{};
            if (!Convert(entry.value(), child, error))
            {
                return false;
            }

            out.keys.push_back(key.text);
            out.children.push_back(std::move(child));
        }

        return true;
    }

    error = "a value of an unknown kind";
    return false;
}

} // namespace

LoadCode ParseConfigText(std::string_view text, ConfigValue& out, std::string& error)
{
    const std::string input{text};
    ConfigValue document{};
    try
    {
        const fkyaml::node root = fkyaml::node::deserialize(input);
        if (!Convert(root, document, error))
        {
            return LoadCode::SyntaxError;
        }
    }
    catch (const fkyaml::parse_error& failure)
    {
        error = DescribeParseError(failure.what(), text);
        return LoadCode::SyntaxError;
    }
    catch (const fkyaml::exception& failure)
    {
        error = failure.what(); // an encoding or tag error: no position is known
        return LoadCode::SyntaxError;
    }

    out = std::move(document);
    return LoadCode::Ok;
}

LoadCode LoadConfigFile(const std::filesystem::path& path, ConfigValue& out, std::string& error)
{
    std::error_code ignored;
    if (!std::filesystem::is_regular_file(path, ignored))
    {
        error = path.string() + " is not a file that can be read";
        return LoadCode::FileNotOpened;
    }

    std::ifstream file{path, std::ios::binary};
    if (!file)
    {
        error = path.string() + " could not be opened";
        return LoadCode::FileNotOpened;
    }

    std::ostringstream content;
    content << file.rdbuf();
    return ParseConfigText(content.str(), out, error);
}

const ConfigValue* FindValue(const ConfigValue& mapping, std::string_view key)
{
    if (mapping.kind != ValueKind::Mapping)
    {
        return nullptr;
    }

    for (std::size_t i = 0; i < mapping.keys.size(); ++i)
    {
        if (mapping.keys[i] == key)
        {
            return &mapping.children[i];
        }
    }

    return nullptr;
}

std::vector<std::string> SplitList(std::string_view text, char separator)
{
    std::vector<std::string> parts;
    while (true)
    {
        const std::size_t end = text.find(separator);
        const std::string_view part = Trim(text.substr(0, end));
        if (!part.empty())
        {
            parts.emplace_back(part);
        }

        if (end == std::string_view::npos)
        {
            return parts;
        }

        text.remove_prefix(end + 1);
    }
}

ConfigSection::ConfigSection(const ConfigValue& value, std::string label)
    : value{&value},
      label{std::move(label)}
{
    if (value.kind != ValueKind::Mapping && value.kind != ValueKind::Null)
    {
        problems.push_back(this->label + ": must be a mapping of keys to values");
    }
}

bool ConfigSection::Has(std::string_view key) const
{
    const ConfigValue* found = FindValue(*value, key);
    return found != nullptr && found->kind != ValueKind::Null;
}

std::string_view ConfigSection::RenamedKey(std::string_view key, std::string_view oldKey)
{
    if (FindValue(*value, oldKey) == nullptr)
    {
        return key;
    }

    Read(oldKey); // reported here instead of as an unknown key
    if (Has(key) && Has(oldKey))
    {
        AddProblem(oldKey, "the old spelling of " + std::string{key} + ", which is set too");
        return key;
    }

    AddProblem(oldKey, "the old spelling of " + std::string{key} + "; it still works, but rename it");
    return Has(key) ? key : oldKey;
}

std::string ConfigSection::ReadString(std::string_view key, std::string_view fallback)
{
    const ConfigValue* found = ReadScalar(key);
    return found != nullptr ? found->text : std::string{fallback};
}

bool ConfigSection::ReadBool(std::string_view key, bool fallback)
{
    const ConfigValue* found = ReadScalar(key);
    if (found == nullptr)
    {
        return fallback;
    }

    for (std::string_view yes : {"true", "yes", "on", "1"})
    {
        if (EqualsIgnoringCase(found->text, yes))
        {
            return true;
        }
    }

    for (std::string_view no : {"false", "no", "off", "0"})
    {
        if (EqualsIgnoringCase(found->text, no))
        {
            return false;
        }
    }

    AddProblem(key, "'" + found->text + "' is not true or false; " + (fallback ? "true" : "false") + " is used");
    return fallback;
}

int ConfigSection::ReadInt(std::string_view key, int fallback, int minimum, int maximum)
{
    const ConfigValue* found = ReadScalar(key);
    if (found == nullptr)
    {
        return fallback;
    }

    std::string_view text = Trim(found->text);
    if (!text.empty() && text.front() == '+')
    {
        text.remove_prefix(1);
    }

    long long number = 0;
    const std::from_chars_result result = std::from_chars(text.data(), text.data() + text.size(), number);
    if (text.empty() || result.ptr != text.data() + text.size() || result.ec == std::errc::invalid_argument)
    {
        AddProblem(key, "'" + found->text + "' is not a whole number; " + std::to_string(fallback) + " is used");
        return fallback;
    }

    if (result.ec == std::errc::result_out_of_range)
    {
        number = text.front() == '-' ? LLONG_MIN : LLONG_MAX;
    }

    if (number < minimum)
    {
        AddProblem(key, found->text + " is below the minimum " + std::to_string(minimum) + ", which is used");
        return minimum;
    }

    if (number > maximum)
    {
        AddProblem(key, found->text + " is above the maximum " + std::to_string(maximum) + ", which is used");
        return maximum;
    }

    return static_cast<int>(number);
}

double ConfigSection::ReadDouble(std::string_view key, double fallback, double minimum, double maximum)
{
    const ConfigValue* found = ReadScalar(key);
    if (found == nullptr)
    {
        return fallback;
    }

    const std::string text{Trim(found->text)};
    char* end = nullptr;
    const double number = std::strtod(text.c_str(), &end);
    if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(number))
    {
        AddProblem(key, "'" + found->text + "' is not a number; " + ShortestText(fallback) + " is used");
        return fallback;
    }

    if (number < minimum)
    {
        AddProblem(key, found->text + " is below the minimum " + ShortestText(minimum) + ", which is used");
        return minimum;
    }

    if (number > maximum)
    {
        AddProblem(key, found->text + " is above the maximum " + ShortestText(maximum) + ", which is used");
        return maximum;
    }

    return number;
}

int ConfigSection::ReadChoice(std::string_view key, std::initializer_list<std::string_view> choices, int fallback)
{
    const ConfigValue* found = ReadScalar(key);
    if (found == nullptr)
    {
        return fallback;
    }

    int index = 0;
    std::string names;
    for (std::string_view choice : choices)
    {
        if (EqualsIgnoringCase(found->text, choice))
        {
            return index;
        }

        names += (index == 0 ? "" : ", ") + std::string{choice};
        ++index;
    }

    std::string used;
    if (fallback >= 0 && static_cast<std::size_t>(fallback) < choices.size())
    {
        used = "; " + std::string{*(choices.begin() + fallback)} + " is used";
    }

    AddProblem(key, "'" + found->text + "' is not one of " + names + used);
    return fallback;
}

std::vector<std::string> ConfigSection::ReadList(std::string_view key, char separator)
{
    const ConfigValue* found = Read(key);
    if (found == nullptr)
    {
        return std::vector<std::string>{};
    }

    if (found->kind == ValueKind::Mapping)
    {
        AddProblem(key, "must be a list or a single value, not a mapping");
        return std::vector<std::string>{};
    }

    if (found->kind != ValueKind::Sequence)
    {
        return SplitList(found->text, separator);
    }

    std::vector<std::string> items;
    for (const ConfigValue& item : found->children)
    {
        if (item.kind == ValueKind::Sequence || item.kind == ValueKind::Mapping || item.kind == ValueKind::Null)
        {
            AddProblem(key, "every list item must be a single value");
            continue;
        }

        items.push_back(item.text);
    }

    return items;
}

std::vector<std::string> ConfigSection::ReadAssignments(std::string_view key, char separator)
{
    const ConfigValue* found = Read(key);
    if (found == nullptr || found->kind != ValueKind::Mapping)
    {
        return ReadList(key, separator);
    }

    std::vector<std::string> assignments;
    for (std::size_t i = 0; i < found->keys.size(); ++i)
    {
        const ConfigValue& assigned = found->children[i];
        if (assigned.kind == ValueKind::Sequence || assigned.kind == ValueKind::Mapping)
        {
            AddProblem(key, "the value of " + found->keys[i] + " must be a single value");
            continue;
        }

        assignments.push_back(found->keys[i] + "=" + assigned.text);
    }

    return assignments;
}

const ConfigValue* ConfigSection::ReadSection(std::string_view key)
{
    const ConfigValue* found = Read(key);
    if (found != nullptr && found->kind != ValueKind::Mapping)
    {
        AddProblem(key, "must be a mapping of keys to values");
        return nullptr;
    }

    return found;
}

const ConfigValue* ConfigSection::ReadSequence(std::string_view key)
{
    const ConfigValue* found = Read(key);
    if (found != nullptr && found->kind != ValueKind::Sequence)
    {
        AddProblem(key, "must be a list");
        return nullptr;
    }

    return found;
}

std::vector<std::string> ConfigSection::Problems() const
{
    std::vector<std::string> all = problems;
    for (const std::string& key : value->keys)
    {
        if (std::find(readKeys.begin(), readKeys.end(), key) == readKeys.end())
        {
            all.push_back(label + ": unknown key '" + key + "'");
        }
    }

    return all;
}

const std::string& ConfigSection::Label() const
{
    return label;
}

const ConfigValue* ConfigSection::Read(std::string_view key)
{
    if (std::find(readKeys.begin(), readKeys.end(), key) == readKeys.end())
    {
        readKeys.emplace_back(key);
    }

    const ConfigValue* found = FindValue(*value, key);
    return found != nullptr && found->kind != ValueKind::Null ? found : nullptr;
}

const ConfigValue* ConfigSection::ReadScalar(std::string_view key)
{
    const ConfigValue* found = Read(key);
    if (found != nullptr && (found->kind == ValueKind::Sequence || found->kind == ValueKind::Mapping))
    {
        AddProblem(key, "must be a single value, not a list or mapping");
        return nullptr;
    }

    return found;
}

void ConfigSection::AddProblem(std::string_view key, std::string_view text)
{
    problems.push_back(label + ": " + std::string{key} + ": " + std::string{text});
}

} // namespace config_section
