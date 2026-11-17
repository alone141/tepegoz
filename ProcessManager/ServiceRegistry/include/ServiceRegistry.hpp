#pragma once

#include "ConfigSection.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace service_registry
{

// The version a "version" request answers with; it grows when the protocol does.
constexpr int protocol_version = 1;
// A registered service's name is also its file name, and a command message holds 31 bytes of a name.
constexpr std::size_t max_name_length = 31;
// A request whose frames are larger than this, together, is refused.
constexpr std::size_t max_request_bytes = 64 * 1024;

enum class Operation
{
    Version,
    List,
    Show,
    Add,
    Replace,
    Update,
    Remove,
};

// One request, as parsed from its frames.
struct Request
{
    Operation operation = Operation::Version;
    // The service it is about; empty for Version and List.
    std::string name;
    // Add, Replace: the keys of the service.
    config_section::ConfigValue definition;
    // Update: the keys to set, with their new values. A mapping, or Null when nothing is set.
    config_section::ConfigValue set;
    // Update: the keys to remove.
    std::vector<std::string> unset;
    // Add: false registers the service without starting it.
    bool start = true;
    // Add, Replace, Update, Remove: check the request and tell what would be written, but write nothing.
    bool dryRun = false;
    // Replace, Update: a running service restarts with its new definition at once.
    bool restart = false;
    // Remove: also while other services depend on it.
    bool force = false;
};

enum class RequestCode
{
    Ok,
    NoOperation,
    UnknownOperation,
    TooLarge,
    TooManyFrames,
    NotYaml,
    NotMapping,
    UnknownParameter,
    WrongKind,
    MissingName,
    MissingDefinition,
    MissingChange,
};

enum class NameCode
{
    Ok,
    Empty,
    TooLong,
    BadCharacter,
    LeadingDot,
};

enum class EditCode
{
    Ok,
    NotMapping,
    NameDiffers,
    NameNotEditable,
};

enum class ReplyStatus
{
    Ok,
    Refused,
    Error,
};

enum class StoreCode
{
    Ok,
    FolderNotMade,
    NotRead,
    NotWritten,
    NotRemoved,
};

// A reply: the word of its first frame, and the JSON object of its second.
struct Reply
{
    ReplyStatus status = ReplyStatus::Ok;
    std::string json;
};

// What a registered service's file and its folder held before a change, so that the change can be undone.
struct Snapshot
{
    std::string name;
    bool folderExisted = false;
    bool fileExisted = false;
    // The file's bytes, when it existed.
    std::string text;
};

// problem is a second out-parameter: what is wrong with the request, worded for whoever sent it.
/**
 * @brief Parse a request: its operation, then its parameters as one YAML or JSON mapping.
 * @param frames The request's frames: the operation, and the parameters when the operation takes any.
 * @param out Receives the request on success.
 * @param problem Receives what is wrong when parsing fails.
 * @return RequestCode::Ok on success, otherwise the failure.
 */
RequestCode ParseRequest(const std::vector<std::string>& frames, Request& out, std::string& problem);

/**
 * @brief Return the word a request names an operation by.
 * @param operation The operation.
 * @return "version", "list", "show", "add", "replace", "update" or "remove".
 */
std::string_view OperationName(Operation operation);

/**
 * @brief Check a name for a registered service, which is also the name of its file.
 * @param name The name.
 * @return NameCode::Ok when it can be used, otherwise why not.
 */
NameCode CheckName(std::string_view name);

/**
 * @brief Describe why a name cannot be used.
 * @param code The result of CheckName.
 * @param name The name that was checked.
 * @return One sentence for whoever chose the name; empty for NameCode::Ok.
 */
std::string DescribeName(NameCode code, std::string_view name);

/**
 * @brief Give a definition its name: set ProcessName, or check the one it has.
 * @param definition The keys of one service. On success its ProcessName is @p name.
 * @param name The service's name.
 * @return EditCode::Ok, NotMapping, or NameDiffers when the definition names another service.
 */
EditCode NameDefinition(config_section::ConfigValue& definition, std::string_view name);

/**
 * @brief Set and remove keys of a definition.
 * @param definition The keys of one service. Left unchanged on failure.
 * @param set The keys to set and their values: a mapping, or Null for none.
 * @param unset The keys to remove; a key that is not there is skipped.
 * @return EditCode::Ok, NotMapping, or NameNotEditable when ProcessName is set or removed.
 */
EditCode ApplyUpdate(config_section::ConfigValue& definition, const config_section::ConfigValue& set,
                     const std::vector<std::string>& unset);

/**
 * @brief Write one service's definition as block-style YAML, ProcessName first.
 * @param definition The keys of one service.
 * @return YAML text that reads back as the same definition.
 */
std::string EmitYaml(const config_section::ConfigValue& definition);

/**
 * @brief Write a value as JSON.
 * @param value Any configuration value. A float that JSON cannot hold is written as a string.
 * @return The JSON text.
 */
std::string EmitJson(const config_section::ConfigValue& value);

/**
 * @brief Write a text as a JSON string.
 * @param text The text.
 * @return The text in double quotes, with quotes, backslashes and control characters escaped.
 */
std::string JsonString(std::string_view text);

/**
 * @brief Return the word a reply's first frame carries.
 * @param status The status.
 * @return "ok", "refused" or "error".
 */
std::string_view StatusText(ReplyStatus status);

/**
 * @brief Find the problems a change brings, by comparing the problems found before and after it.
 * @param before The problems of the configuration as it is.
 * @param after The problems of the configuration the change would make.
 * @return The problems of @p after that @p before does not have, each as often as it is new.
 */
std::vector<std::string> NewProblems(const std::vector<std::string>& before, const std::vector<std::string>& after);

// The JSON object a reply carries, written field by field.
class JsonObject
{
public:
    /**
     * @brief Add a field whose value is a text.
     * @param key The field's name.
     * @param value The text.
     */
    void AddText(std::string_view key, std::string_view value);

    /**
     * @brief Add a field whose value is a whole number.
     * @param key The field's name.
     * @param value The number.
     */
    void AddNumber(std::string_view key, long long value);

    /**
     * @brief Add a field whose value is true or false.
     * @param key The field's name.
     * @param value The value.
     */
    void AddFlag(std::string_view key, bool value);

    /**
     * @brief Add a field whose value is a list of texts.
     * @param key The field's name.
     * @param values The texts.
     */
    void AddTexts(std::string_view key, const std::vector<std::string>& values);

    /**
     * @brief Add a field whose value is already written as JSON.
     * @param key The field's name.
     * @param json The value's JSON text.
     */
    void AddJson(std::string_view key, std::string_view json);

    /**
     * @brief Return the object.
     * @return The fields in the order they were added, in braces.
     */
    std::string Text() const;

private:
    std::string fields;
};

// The files of registered services: one <name>.yaml each in one folder.
class ServiceFiles
{
public:
    /**
     * @brief Store the folder that holds the files.
     * @param folder The folder. It need not exist yet.
     */
    explicit ServiceFiles(std::filesystem::path folder);

    /**
     * @brief Return the file of a service.
     * @param name The service's name.
     * @return The folder joined with <name>.yaml.
     */
    std::filesystem::path FileOf(std::string_view name) const;

    /**
     * @brief Tell whether anything of the file's name is in the folder: a file, a folder, a broken link.
     * @param name The service's name.
     * @return true when something of that name is there.
     */
    bool Exists(std::string_view name) const;

    /**
     * @brief Read a service's file.
     * @param name The service's name.
     * @param text Receives the file's bytes on success.
     * @return StoreCode::Ok, or NotRead.
     */
    StoreCode Read(std::string_view name, std::string& text) const;

    /**
     * @brief Record what a service's file and the folder hold now, for Restore.
     * @param name The service's name.
     * @param out Receives the record on success.
     * @return StoreCode::Ok, or NotRead when the file is there but cannot be read.
     */
    StoreCode Save(std::string_view name, Snapshot& out) const;

    /**
     * @brief Write a service's file: to a dot file in the folder first, then renamed over the file.
     * @param name The service's name.
     * @param text The file's content.
     * @return StoreCode::Ok, otherwise FolderNotMade or NotWritten; the file is then as it was.
     */
    StoreCode Write(std::string_view name, std::string_view text) const;

    /**
     * @brief Delete a service's file.
     * @param name The service's name.
     * @return StoreCode::Ok, also when there was no file, or NotRemoved.
     */
    StoreCode Remove(std::string_view name) const;

    /**
     * @brief Put a service's file and the folder back as a Snapshot recorded them.
     * @param snapshot What Save recorded.
     * @return StoreCode::Ok, otherwise the failure.
     */
    StoreCode Restore(const Snapshot& snapshot) const;

    /**
     * @brief Return the folder.
     * @return The folder given to the constructor.
     */
    const std::filesystem::path& Folder() const;

private:
    std::filesystem::path folder;
};

} // namespace service_registry
