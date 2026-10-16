#pragma once

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace config_section
{

enum class ValueKind
{
    Null,
    Boolean,
    Integer,
    Float,
    String,
    Sequence,
    Mapping,
};

// One node of a configuration document.
struct ConfigValue
{
    ValueKind kind = ValueKind::Null;
    // Scalars: the value as text, such as "1000", "true" or ":1".
    std::string text;
    // Mapping: its keys, sorted; children[i] is the value of keys[i].
    std::vector<std::string> keys;
    // Sequence: the items. Mapping: the values.
    std::vector<ConfigValue> children;
};

enum class LoadCode
{
    Ok,
    FileNotOpened,
    SyntaxError,
};

// error is a second out-parameter: the parser's explanation, worded for whoever edits the file.
/**
 * @brief Parse the YAML document @p text into @p out.
 * @param text The document.
 * @param out Receives the document on success. An empty document is Null.
 * @param error Receives "line L, column C: what is wrong" on failure.
 * @return LoadCode::Ok, otherwise SyntaxError.
 */
LoadCode ParseConfigText(std::string_view text, ConfigValue& out, std::string& error);

// error is a second out-parameter: the parser's explanation, worded for whoever edits the file.
/**
 * @brief Read the YAML file @p path into @p out.
 * @param path File to read.
 * @param out Receives the document on success.
 * @param error Receives why the file could not be read or parsed.
 * @return LoadCode::Ok, otherwise FileNotOpened or SyntaxError.
 */
LoadCode LoadConfigFile(const std::filesystem::path& path, ConfigValue& out, std::string& error);

// A pointer, not std::optional: callers read the value in place instead of copying its subtree.
/**
 * @brief Find the value of @p key in @p mapping.
 * @param mapping Value to search. Anything but a mapping has no keys.
 * @param key Key to look for.
 * @return The value, or nullptr when the key is absent.
 */
const ConfigValue* FindValue(const ConfigValue& mapping, std::string_view key);

/**
 * @brief Split @p text at @p separator, dropping blanks around the parts and empty parts.
 * @param text Text such as "a b  c" or "A=1;B=2".
 * @param separator Character between the parts.
 * @return The parts, in order.
 */
std::vector<std::string> SplitList(std::string_view text, char separator);

// Reads typed settings from one mapping, falling back to defaults, and keeps a list of the
// problems it met: values of the wrong kind, values out of range and keys nothing read.
class ConfigSection
{
public:
    /**
     * @brief Read settings from @p value.
     * @param value The mapping. Null reads as an empty mapping; anything else is a problem.
     * @param label Names the section in problems, such as "service 'keepalive'".
     */
    ConfigSection(const ConfigValue& value, std::string label);

    /**
     * @brief Tell whether @p key has a value. Asking does not count as reading it.
     * @param key Key to look for.
     * @return true when the key is present and not null.
     */
    bool Has(std::string_view key) const;

    /**
     * @brief Read a single value as text.
     * @param key Key to read.
     * @param fallback Returned when the key is missing or holds a list or mapping.
     * @return The text: numbers and booleans in their resolved form, such as "16" for 0x10.
     */
    std::string ReadString(std::string_view key, std::string_view fallback);

    /**
     * @brief Read a boolean: true, false, yes, no, on, off, 1 or 0, in any case.
     * @param key Key to read.
     * @param fallback Returned when the key is missing or holds anything else.
     * @return The value.
     */
    bool ReadBool(std::string_view key, bool fallback);

    /**
     * @brief Read a whole number, kept within [@p minimum, @p maximum].
     * @param key Key to read.
     * @param fallback Returned when the key is missing or not a whole number.
     * @param minimum Smallest value kept; a smaller one is raised to it.
     * @param maximum Largest value kept; a larger one is lowered to it.
     * @return The value.
     */
    int ReadInt(std::string_view key, int fallback, int minimum, int maximum);

    /**
     * @brief Read a number, kept within [@p minimum, @p maximum].
     * @param key Key to read.
     * @param fallback Returned when the key is missing or not a number.
     * @param minimum Smallest value kept; a smaller one is raised to it.
     * @param maximum Largest value kept; a larger one is lowered to it.
     * @return The value.
     */
    double ReadDouble(std::string_view key, double fallback, double minimum, double maximum);

    /**
     * @brief Read one of @p choices, compared without case.
     * @param key Key to read.
     * @param choices The accepted spellings.
     * @param fallback Index returned when the key is missing or holds something else.
     * @return The index of the choice in @p choices.
     */
    int ReadChoice(std::string_view key, std::initializer_list<std::string_view> choices, int fallback);

    /**
     * @brief Read a list: a sequence of single values, or one text split at @p separator.
     * @param key Key to read.
     * @param separator Splits a text value, such as ' ' for "--port 1001".
     * @return The items. Missing or null gives an empty list.
     */
    std::vector<std::string> ReadList(std::string_view key, char separator);

    /**
     * @brief Read NAME=VALUE assignments: a mapping, a sequence of NAME=VALUE, or one text split at @p separator.
     * @param key Key to read.
     * @param separator Splits a text value, such as ';' for "A=1;B=2".
     * @return The assignments as NAME=VALUE texts.
     */
    std::vector<std::string> ReadAssignments(std::string_view key, char separator);

    // A pointer, not std::optional: callers read the mapping in place instead of copying it.
    /**
     * @brief Read a nested mapping.
     * @param key Key to read.
     * @return The mapping, or nullptr when it is missing, null or not a mapping.
     */
    const ConfigValue* ReadSection(std::string_view key);

    // A pointer, not std::optional: callers read the sequence in place instead of copying it.
    /**
     * @brief Read a sequence.
     * @param key Key to read.
     * @return The sequence, or nullptr when it is missing, null or not a sequence.
     */
    const ConfigValue* ReadSequence(std::string_view key);

    /**
     * @brief List the problems met so far, and the keys nothing has read.
     * @return One text per problem, each starting with the section's label.
     */
    std::vector<std::string> Problems() const;

    /**
     * @brief Return the label problems start with.
     * @return The label given to the constructor.
     */
    const std::string& Label() const;

private:
    const ConfigValue* Read(std::string_view key);
    const ConfigValue* ReadScalar(std::string_view key);
    void AddProblem(std::string_view key, std::string_view text);

    const ConfigValue* value;
    std::string label;
    std::vector<std::string> readKeys;
    std::vector<std::string> problems;
};

} // namespace config_section
