#include "ConfigSource.hpp"

#include "ConfigSection.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace config_source
{

namespace
{

constexpr std::string_view services_key = "Services";
constexpr std::string_view name_key = "ProcessName";
constexpr std::string_view skipped = "; file skipped";

bool IsSingleValue(const config_section::ConfigValue& value)
{
    return value.kind != config_section::ValueKind::Null && value.kind != config_section::ValueKind::Sequence &&
           value.kind != config_section::ValueKind::Mapping;
}

// Empty when the item has no ProcessName, or one that is not a single value.
std::string NameOf(const config_section::ConfigValue& item)
{
    const config_section::ConfigValue* name = config_section::FindValue(item, name_key);
    if (name == nullptr || !IsSingleValue(*name))
    {
        return std::string{};
    }

    return name->text;
}

// A pointer, not std::optional: the list is read in place instead of copied.
const config_section::ConfigValue* MainServices(const config_section::ConfigValue& document)
{
    const config_section::ConfigValue* services = config_section::FindValue(document, services_key);
    if (services == nullptr || services->kind != config_section::ValueKind::Sequence)
    {
        return nullptr;
    }

    return services;
}

// Whether drop-in services can be appended: an empty document, or a mapping whose Services is missing, empty or a list.
bool CanTakeServices(const config_section::ConfigValue& document)
{
    if (document.kind == config_section::ValueKind::Null)
    {
        return true;
    }

    if (document.kind != config_section::ValueKind::Mapping)
    {
        return false;
    }

    const config_section::ConfigValue* services = config_section::FindValue(document, services_key);
    return services == nullptr || services->kind == config_section::ValueKind::Null ||
           services->kind == config_section::ValueKind::Sequence;
}

// Dot files are a writer's temporary files.
bool IsDropInName(const std::filesystem::path& name)
{
    const std::string text = name.string();
    return !text.empty() && text.front() != '.' && name.extension() == drop_in_extension;
}

// problems is a second output: a folder that cannot be read is reported, and the load goes on without it.
// Puts the drop-in files of folder into files, in sorted name order. A folder that is missing has none, and says
// nothing.
FolderCode ListDropIns(const std::filesystem::path& folder, std::vector<std::filesystem::path>& files,
                       std::vector<std::string>& problems)
{
    std::error_code error{};
    const std::filesystem::file_status status = std::filesystem::status(folder, error);
    if (status.type() == std::filesystem::file_type::not_found)
    {
        return FolderCode::Missing;
    }

    if (error)
    {
        problems.push_back(folder.string() + ": cannot be read (" + error.message() +
                           "); no registered service is read");
        return FolderCode::NotReadable;
    }

    if (status.type() != std::filesystem::file_type::directory)
    {
        problems.push_back(folder.string() + ": is not a folder; no registered service is read");
        return FolderCode::NotFolder;
    }

    std::filesystem::directory_iterator entry{folder, error};
    for (; !error && entry != std::filesystem::directory_iterator{}; entry.increment(error))
    {
        std::error_code ignored{};
        if (!entry->is_directory(ignored) && IsDropInName(entry->path().filename()))
        {
            files.push_back(entry->path());
        }
    }

    if (error)
    {
        problems.push_back(folder.string() + ": cannot be listed (" + error.message() +
                           "); no registered service is read");
        files.clear();
        return FolderCode::NotReadable;
    }

    std::sort(files.begin(), files.end(),
              [](const std::filesystem::path& a, const std::filesystem::path& b)
              {
                  return a.filename().string() < b.filename().string();
              });
    return FolderCode::Listed;
}

// problem is a second out-parameter: why the file is skipped, worded for whoever edits it.
DropInCode ReadDropIn(const std::filesystem::path& file, const std::vector<std::string>& mainNames,
                      config_section::ConfigValue& service, std::string& problem)
{
    const std::string label = file.string();
    const std::string serviceName = file.stem().string();
    std::string error;
    const config_section::LoadCode loaded = config_section::LoadConfigFile(file, service, error);
    if (loaded == config_section::LoadCode::FileNotOpened)
    {
        problem = label + ": cannot be read" + std::string{skipped};
        return DropInCode::NotReadable;
    }

    if (loaded == config_section::LoadCode::SyntaxError)
    {
        problem = label + ": not valid YAML: " + error + std::string{skipped};
        return DropInCode::SyntaxError;
    }

    if (service.kind != config_section::ValueKind::Mapping)
    {
        problem = label + ": must be a mapping of keys to values, the keys of one service" + std::string{skipped};
        return DropInCode::NotMapping;
    }

    const std::string name = NameOf(service);
    if (name.empty())
    {
        problem = label + ": ProcessName is missing; the file name says '" + serviceName + "'" + std::string{skipped};
        return DropInCode::NameMismatch;
    }

    if (name != serviceName)
    {
        problem = label + ": ProcessName is '" + name + "', but the file name says '" + serviceName + "'" +
                  std::string{skipped};
        return DropInCode::NameMismatch;
    }

    if (std::find(mainNames.begin(), mainNames.end(), name) != mainNames.end())
    {
        problem = label + ": the main file already defines service '" + name + "', which is used instead" +
                  std::string{skipped};
        return DropInCode::NameTaken;
    }

    return DropInCode::Ok;
}

// Appends services to the Services list of document, making the list when the document has none.
void AppendServices(config_section::ConfigValue& document, std::vector<config_section::ConfigValue>& services)
{
    if (document.kind == config_section::ValueKind::Null)
    {
        document.kind = config_section::ValueKind::Mapping;
    }

    const std::vector<std::string>::iterator found =
        std::find(document.keys.begin(), document.keys.end(), services_key);
    std::size_t index = static_cast<std::size_t>(found - document.keys.begin());
    if (found == document.keys.end())
    {
        // Keys stay sorted, as the reader gives them.
        const std::vector<std::string>::iterator at =
            std::lower_bound(document.keys.begin(), document.keys.end(), services_key);
        index = static_cast<std::size_t>(at - document.keys.begin());
        document.keys.insert(at, std::string{services_key});
        document.children.insert(document.children.begin() + static_cast<std::ptrdiff_t>(index),
                                 config_section::ConfigValue{});
    }

    config_section::ConfigValue& list = document.children[index];
    list.kind = config_section::ValueKind::Sequence;
    for (config_section::ConfigValue& service : services)
    {
        list.children.push_back(std::move(service));
    }
}

} // namespace

ConfigSource::ConfigSource(std::filesystem::path mainFile)
    : mainFile{std::move(mainFile)}
{
}

LoadCode ConfigSource::Load(LoadedConfig& out, std::string& error) const
{
    LoadedConfig loaded{};
    loaded.mainFile = mainFile;
    const config_section::LoadCode read = config_section::LoadConfigFile(mainFile, loaded.document, error);
    if (read == config_section::LoadCode::FileNotOpened)
    {
        return LoadCode::MainFileNotOpened;
    }

    if (read == config_section::LoadCode::SyntaxError)
    {
        return LoadCode::MainFileSyntaxError;
    }

    const config_section::ConfigValue* mainServices = MainServices(loaded.document);
    std::vector<std::string> mainNames;
    for (std::size_t i = 0; mainServices != nullptr && i < mainServices->children.size(); ++i)
    {
        const std::string name = NameOf(mainServices->children[i]);
        loaded.origins.push_back(ServiceOrigin{name, OriginKind::MainFile, mainFile});
        mainNames.push_back(name);
    }

    const std::filesystem::path folder = DropInFolder();
    std::vector<std::filesystem::path> files;
    loaded.folder = ListDropIns(folder, files, loaded.problems);
    loaded.takesServices = CanTakeServices(loaded.document);
    if (!files.empty() && !loaded.takesServices)
    {
        // The main file's own problem is reported where its document is read.
        loaded.problems.push_back(folder.string() +
                                  ": no registered service is read, because the main file has no Services list to "
                                  "add them to");
        out = std::move(loaded);
        return LoadCode::Ok;
    }

    std::vector<config_section::ConfigValue> added;
    for (const std::filesystem::path& file : files)
    {
        config_section::ConfigValue service{};
        std::string problem;
        const DropInCode code = ReadDropIn(file, mainNames, service, problem);
        loaded.dropIns.push_back(DropInFile{file, file.stem().string(), code});
        if (code != DropInCode::Ok)
        {
            loaded.problems.push_back(problem);
            continue;
        }

        loaded.origins.push_back(ServiceOrigin{file.stem().string(), OriginKind::DropIn, file});
        added.push_back(std::move(service));
    }

    if (!added.empty())
    {
        AppendServices(loaded.document, added);
    }

    out = std::move(loaded);
    return LoadCode::Ok;
}

const std::filesystem::path& ConfigSource::MainFile() const
{
    return mainFile;
}

std::filesystem::path ConfigSource::DropInFolder() const
{
    return mainFile.parent_path() / std::filesystem::path{drop_in_folder};
}

const ServiceOrigin* FindOrigin(const std::vector<ServiceOrigin>& origins, std::string_view processName)
{
    for (const ServiceOrigin& origin : origins)
    {
        if (origin.processName == processName)
        {
            return &origin;
        }
    }

    return nullptr;
}

} // namespace config_source
