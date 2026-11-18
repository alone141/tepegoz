#pragma once

#include "ConfigSection.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace config_source
{

// The folder beside the main configuration file that holds one file per registered service.
constexpr std::string_view drop_in_folder = "services.d";
// The extension of the files read from that folder; anything else there is ignored.
constexpr std::string_view drop_in_extension = ".yaml";

enum class OriginKind
{
    MainFile,
    DropIn,
};

// Where one item of the merged Services list was defined.
struct ServiceOrigin
{
    // Its ProcessName; empty when the item has none.
    std::string processName;
    OriginKind kind = OriginKind::MainFile;
    // The main file, or the drop-in file.
    std::filesystem::path file;
};

enum class DropInCode
{
    Ok,
    NotReadable,
    SyntaxError,
    NotMapping,
    NameMismatch,
    NameTaken,
};

// A drop-in file that was read, whether its service was added or the file was skipped.
struct DropInFile
{
    std::filesystem::path file;
    // The file name without its extension: the ProcessName the file must define.
    std::string serviceName;
    DropInCode code = DropInCode::Ok;
};

// A registered service's file as a change would leave it, so that the change can be checked before it is written.
struct DropInOverride
{
    // The service, which is also the file's name without its extension.
    std::string serviceName;
    // false: the file is taken as deleted.
    bool present = true;
    // The file's content when present: the keys of one service.
    config_section::ConfigValue definition;
};

// What became of the drop-in folder. An empty folder is Listed; one that could not be listed has no files either, but
// is NotReadable or NotFolder, so that it is never taken for a folder whose files were all deleted.
enum class FolderCode
{
    Listed,
    Missing,
    NotReadable,
    NotFolder,
};

// The configuration as the manager uses it: the main file and its drop-in folder, read together.
struct LoadedConfig
{
    // The main file that was read.
    std::filesystem::path mainFile;
    // The main file's document, with the service of every drop-in file that was added appended to its Services.
    config_section::ConfigValue document;
    // One per item of the merged Services list, in the same order.
    std::vector<ServiceOrigin> origins;
    // Every drop-in file that was read, in the order it was read.
    std::vector<DropInFile> dropIns;
    // Problems with the drop-in folder and its files. Problems inside the services are left to their readers.
    std::vector<std::string> problems;
    // Whether the drop-in folder was listed. Its files are read only when it was.
    FolderCode folder = FolderCode::Missing;
    // Whether the main file can take the registered services: it is empty, or a mapping whose Services is missing,
    // empty or a list. When it cannot, no drop-in file is read.
    bool takesServices = true;
};

enum class LoadCode
{
    Ok,
    MainFileNotOpened,
    MainFileSyntaxError,
};

// Reads the main configuration file and the drop-in folder beside it, as often as asked.
class ConfigSource
{
public:
    /**
     * @brief Store the path of the main configuration file.
     * @param mainFile The main file. Its folder holds the drop-in folder.
     */
    explicit ConfigSource(std::filesystem::path mainFile);

    // error is a second out-parameter: the reason the main file failed, worded for whoever edits it.
    /**
     * @brief Read the main file and the drop-in folder beside it into @p out.
     * @param out Receives the merged configuration on success. A drop-in problem skips that file only.
     * @param error Receives why the main file could not be read or parsed.
     * @return LoadCode::Ok, otherwise MainFileNotOpened or MainFileSyntaxError.
     */
    LoadCode Load(LoadedConfig& out, std::string& error) const;

    // error is a second out-parameter: the reason the main file failed, worded for whoever edits it.
    /**
     * @brief Read the configuration as it would be with some files of the drop-in folder written or deleted.
     * @param out Receives the merged configuration on success. A drop-in problem skips that file only.
     * @param error Receives why the main file could not be read or parsed.
     * @param overrides The files taken as written or deleted, by service name. Nothing on disk is touched, and a
     *        folder that cannot be listed takes none of them.
     * @return LoadCode::Ok, otherwise MainFileNotOpened or MainFileSyntaxError.
     */
    LoadCode Load(LoadedConfig& out, std::string& error, const std::vector<DropInOverride>& overrides) const;

    /**
     * @brief Return the path of the main configuration file.
     * @return The path given to the constructor.
     */
    const std::filesystem::path& MainFile() const;

    /**
     * @brief Return the drop-in folder: services.d in the main file's folder.
     * @return The folder's path. It need not exist.
     */
    std::filesystem::path DropInFolder() const;

private:
    std::filesystem::path mainFile;
};

// A pointer, not std::optional: callers read the origin in place instead of copying it.
/**
 * @brief Find the origin of the service named @p processName.
 * @param origins The origins of a merged Services list.
 * @param processName The ProcessName to look for.
 * @return The first origin with that name, or nullptr when there is none.
 */
const ServiceOrigin* FindOrigin(const std::vector<ServiceOrigin>& origins, std::string_view processName);

} // namespace config_source
