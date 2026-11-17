#include "ProcessManager.hpp"

#include "ConfigSection.hpp"
#include "ConfigSource.hpp"
#include "Logger.hpp"
#include "ProcessConfigParams.hpp"
#include "ServiceRegistry.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <zmq.hpp>
#include <zmq_addon.hpp>

namespace process_manager
{

namespace
{

constexpr int max_requests_per_cycle = 16;
constexpr std::string_view services_key = "Services";
constexpr std::string_view name_key = "ProcessName";

// What a written change needs for its reply.
struct Written
{
    std::string file;
    std::string yaml;
    bool mainDefines = false;
    bool wasLoaded = false;
    bool wasRunning = false;
    ReloadReport report{};
};

std::string Quoted(const std::string& name)
{
    return "'" + name + "'";
}

service_registry::JsonObject Header(service_registry::Operation operation)
{
    service_registry::JsonObject object{};
    object.AddNumber("version", service_registry::protocol_version);
    object.AddText("operation", service_registry::OperationName(operation));
    return object;
}

service_registry::Reply Refuse(service_registry::Operation operation, const std::string& name,
                               const std::string& message, const std::vector<std::string>& problems)
{
    const std::string operationName{service_registry::OperationName(operation)};
    logger::Warning("Registry: %s %s refused: %s", operationName.c_str(), name.c_str(), message.c_str());
    for (const std::string& problem : problems)
    {
        logger::Warning("Registry:   %s", problem.c_str());
    }

    service_registry::JsonObject object = Header(operation);
    if (!name.empty())
    {
        object.AddText("name", name);
    }

    object.AddText("message", message);
    object.AddTexts("problems", problems);
    return service_registry::Reply{service_registry::ReplyStatus::Refused, object.Text()};
}

const std::vector<config_section::ConfigValue>& ServicesOf(const config_section::ConfigValue& document)
{
    static const std::vector<config_section::ConfigValue> none{};
    const config_section::ConfigValue* services = config_section::FindValue(document, services_key);
    if (services == nullptr || services->kind != config_section::ValueKind::Sequence)
    {
        return none;
    }

    return services->children;
}

// Empty when the entry has no ProcessName, or one that is not a single value.
std::string NameOf(const config_section::ConfigValue& entry)
{
    const config_section::ConfigValue* name = config_section::FindValue(entry, name_key);
    if (name == nullptr || name->kind == config_section::ValueKind::Null ||
        name->kind == config_section::ValueKind::Sequence || name->kind == config_section::ValueKind::Mapping)
    {
        return std::string{};
    }

    return name->text;
}

// Read as the manager reads it: anything but a boolean leaves the service on.
bool IsEnabled(const config_section::ConfigValue& entry)
{
    config_section::ConfigSection section{entry, "service"};
    return section.ReadBool("Enabled", true);
}

// A pointer, not std::optional: the caller acts on the service in the list.
Process* FindProcess(ProcessManagerParams& params, std::string_view name)
{
    for (Process& process : params.processList)
    {
        if (process.GetProcessName() == name)
        {
            return &process;
        }
    }

    return nullptr;
}

bool MainDefines(const config_source::LoadedConfig& loaded, std::string_view name)
{
    return std::any_of(loaded.origins.begin(), loaded.origins.end(),
                       [name](const config_source::ServiceOrigin& origin)
                       {
                           return origin.kind == config_source::OriginKind::MainFile && origin.processName == name;
                       });
}

// The problems validate reports for a configuration: its files, its keys and values, its order and its paths.
std::vector<std::string> ProblemsOf(const config_source::LoadedConfig& loaded)
{
    std::vector<std::string> problems = loaded.problems;
    const std::unique_ptr<ProcessManagerParams> built = ProcessManager::GenerateConfig(loaded.document, &problems);
    ProcessManager::SortByDependencies(problems, built->processList);
    for (const Process& process : built->processList)
    {
        process.CheckConfig(problems);
    }

    return problems;
}

// The enabled services whose DependsOn names the service.
std::vector<std::string> DependentsOf(const config_source::LoadedConfig& loaded, const std::string& name)
{
    std::vector<std::string> dependents;
    for (const config_section::ConfigValue& entry : ServicesOf(loaded.document))
    {
        const std::string entryName = NameOf(entry);
        if (entryName.empty() || entryName == name || !IsEnabled(entry))
        {
            continue;
        }

        config_section::ConfigSection section{entry, entryName};
        const std::vector<std::string> dependsOn = section.ReadList("DependsOn", ' ');
        if (std::find(dependsOn.begin(), dependsOn.end(), name) != dependsOn.end())
        {
            dependents.push_back(entryName);
        }
    }

    return dependents;
}

// The loader's text for a file it skipped.
std::string ProblemOf(const config_source::LoadedConfig& loaded, const config_source::DropInFile& file)
{
    const std::string prefix = file.file.string() + ":";
    for (const std::string& problem : loaded.problems)
    {
        if (problem.rfind(prefix, 0) == 0)
        {
            return problem;
        }
    }

    return prefix + " skipped";
}

std::string DescribeReload(ReloadResult result)
{
    switch (result)
    {
    case ReloadResult::NoSource:
        return "the configuration was not read from a file";
    case ReloadResult::MainFileNotOpened:
        return "the main file could not be opened";
    case ReloadResult::MainFileSyntaxError:
        return "the main file is not valid YAML";
    case ReloadResult::NoServicesList:
        return "the main file has no Services list";
    case ReloadResult::FolderNotReadable:
        return "services.d could not be listed";
    case ReloadResult::DependencyCycle:
        return "the configuration would have a dependency cycle";
    case ReloadResult::DuplicateName:
        return "the configuration would have two services of the same name";
    default:
        return "it was applied";
    }
}

std::string ReloadCounts(const ReloadReport& report)
{
    service_registry::JsonObject counts{};
    counts.AddNumber("added", report.added);
    counts.AddNumber("removed", report.removed);
    counts.AddNumber("changed", report.changed);
    counts.AddNumber("kept", report.kept);
    counts.AddNumber("skipped", report.skipped);
    counts.AddNumber("unchanged", report.unchanged);
    return counts.Text();
}

void AddRunState(service_registry::JsonObject& object, ProcessManagerParams& params, std::string_view name)
{
    const Process* process = FindProcess(params, name);
    object.AddFlag("loaded", process != nullptr);
    object.AddText("state", process != nullptr ? RuntimeStateToString(process->GetRuntimeState()) : "NotLoaded");
    object.AddNumber("pid", process != nullptr && process->HasPid() ? process->GetPid() : -1);
    object.AddText("workMode", process == nullptr ? "" : (process->IsWorkingManually() ? "Manual" : "Auto"));
    object.AddFlag("restartToApply", process != nullptr && process->HasDeferredDefinition());
}

std::string OriginWord(config_source::OriginKind kind)
{
    return kind == config_source::OriginKind::MainFile ? "main" : "registered";
}

service_registry::Reply ReplyVersion(const ProcessManagerParams& params)
{
    service_registry::JsonObject object = Header(service_registry::Operation::Version);
    object.AddText("message", "service registry, protocol version " + std::to_string(service_registry::protocol_version));
    object.AddText("folder", params.source.DropInFolder().string());
    return service_registry::Reply{service_registry::ReplyStatus::Ok, object.Text()};
}

service_registry::Reply ReplyList(ProcessManagerParams& params)
{
    const service_registry::Operation operation = service_registry::Operation::List;
    config_source::LoadedConfig loaded{};
    std::string error;
    if (params.source.MainFile().empty() || params.source.Load(loaded, error) != config_source::LoadCode::Ok)
    {
        return Refuse(operation, "", "the configuration cannot be read" + (error.empty() ? "" : ": " + error), {});
    }

    std::string services{"["};
    const std::vector<config_section::ConfigValue>& entries = ServicesOf(loaded.document);
    for (std::size_t i = 0; i < loaded.origins.size() && i < entries.size(); ++i)
    {
        const config_source::ServiceOrigin& origin = loaded.origins[i];
        service_registry::JsonObject item{};
        item.AddText("name", origin.processName);
        item.AddText("origin", OriginWord(origin.kind));
        item.AddText("file", origin.file.string());
        item.AddFlag("enabled", IsEnabled(entries[i]));
        AddRunState(item, params, origin.processName);
        services += (services.size() > 1 ? "," : "") + item.Text();
    }

    for (const config_source::DropInFile& file : loaded.dropIns)
    {
        if (file.code == config_source::DropInCode::Ok)
        {
            continue;
        }

        service_registry::JsonObject item{};
        item.AddText("name", file.serviceName);
        item.AddText("origin", "registered");
        item.AddText("file", file.file.string());
        item.AddFlag("enabled", false);
        item.AddText("problem", ProblemOf(loaded, file));
        if (file.code != config_source::DropInCode::NameTaken)
        {
            AddRunState(item, params, file.serviceName); // a broken file keeps its service as it runs
        }

        services += (services.size() > 1 ? "," : "") + item.Text();
    }

    service_registry::JsonObject object = Header(operation);
    const std::size_t count = loaded.origins.size();
    object.AddText("message", std::to_string(count) + (count == 1 ? " service" : " services"));
    object.AddText("folder", params.source.DropInFolder().string());
    object.AddJson("services", services + "]");
    return service_registry::Reply{service_registry::ReplyStatus::Ok, object.Text()};
}

service_registry::Reply ReplyShow(ProcessManagerParams& params, const service_registry::Request& request)
{
    const service_registry::Operation operation = service_registry::Operation::Show;
    const std::string& name = request.name;
    config_source::LoadedConfig loaded{};
    std::string error;
    if (params.source.MainFile().empty() || params.source.Load(loaded, error) != config_source::LoadCode::Ok)
    {
        return Refuse(operation, name, "the configuration cannot be read" + (error.empty() ? "" : ": " + error), {});
    }

    const service_registry::ServiceFiles files{params.source.DropInFolder()};
    service_registry::JsonObject object = Header(operation);
    object.AddText("name", name);
    const std::vector<config_section::ConfigValue>& entries = ServicesOf(loaded.document);
    for (std::size_t i = 0; i < loaded.origins.size() && i < entries.size(); ++i)
    {
        const config_source::ServiceOrigin& origin = loaded.origins[i];
        if (origin.processName != name)
        {
            continue;
        }

        object.AddText("message", Quoted(name) + " is " +
                                      (origin.kind == config_source::OriginKind::MainFile ? "defined in the main file"
                                                                                          : "registered"));
        object.AddText("origin", OriginWord(origin.kind));
        object.AddText("file", origin.file.string());
        object.AddFlag("enabled", IsEnabled(entries[i]));
        object.AddJson("definition", service_registry::EmitJson(entries[i]));
        object.AddText("yaml", service_registry::EmitYaml(entries[i]));
        AddRunState(object, params, name);
        return service_registry::Reply{service_registry::ReplyStatus::Ok, object.Text()};
    }

    for (const config_source::DropInFile& file : loaded.dropIns)
    {
        if (file.serviceName != name)
        {
            continue;
        }

        // A file that was skipped: shown as it is, so that it can be repaired with replace or removed.
        std::string text;
        object.AddText("message", ProblemOf(loaded, file));
        object.AddText("origin", "registered");
        object.AddText("file", file.file.string());
        object.AddJson("definition", "null");
        object.AddText("problem", ProblemOf(loaded, file));
        object.AddText("fileText", files.Read(name, text) == service_registry::StoreCode::Ok ? text : "");
        AddRunState(object, params, name);
        return service_registry::Reply{service_registry::ReplyStatus::Ok, object.Text()};
    }

    return Refuse(operation, name, "there is no service named " + Quoted(name), {});
}

service_registry::Reply ReplyWritten(ProcessManagerParams& params, const service_registry::Request& request,
                                     const Written& written)
{
    const std::string& name = request.name;
    Process* process = FindProcess(params, name);
    std::string outcome;
    std::string message;
    if (request.operation == service_registry::Operation::Remove)
    {
        outcome = "removed";
        message = written.mainDefines ? written.file + " is removed; " + Quoted(name) + " stays as the main file defines it"
                                      : Quoted(name) + " is removed" + (written.wasRunning ? " and stops" : "");
    }
    else if (process == nullptr)
    {
        outcome = "disabled";
        message = Quoted(name) + " is registered and switched off (Enabled: false)" +
                  (written.wasRunning ? "; it stops" : "");
    }
    else if (!written.wasLoaded)
    {
        if (request.operation == service_registry::Operation::Add && !request.start)
        {
            process->SetWorkMode(WorkMode::Manual); // in the manager only: the file keeps its own work mode
            outcome = "held";
            message = Quoted(name) + " is registered and held: start it with --start " + name;
        }
        else if (process->IsWorkingManually())
        {
            outcome = "manual";
            message = Quoted(name) + " is registered; its WorkMode is Manual, so start it with --start " + name;
        }
        else
        {
            outcome = "starting";
            message = Quoted(name) + " is registered and starts";
        }
    }
    else if (process->HasDeferredDefinition())
    {
        if (!request.restart)
        {
            outcome = "waiting";
            message = Quoted(name) + (process->HasPid() ? " keeps running; its new definition takes effect when it restarts"
                                                        : " takes its new definition at its next start");
        }
        else if (process->HasPid())
        {
            process->Restart();
            outcome = "restarting";
            message = Quoted(name) + " restarts with its new definition";
        }
        else
        {
            process->ApplyDeferredDefinition();
            outcome = "applied";
            message = Quoted(name) + " is not running; its new definition is in place for its next start";
        }
    }
    else
    {
        outcome = "unchanged";
        message = Quoted(name) + " has that definition already";
    }

    const std::string operationName{service_registry::OperationName(request.operation)};
    logger::Info("Registry: %s %s: %s", operationName.c_str(), name.c_str(), message.c_str());
    service_registry::JsonObject object = Header(request.operation);
    object.AddText("name", name);
    object.AddText("outcome", outcome);
    object.AddText("message", message);
    object.AddText("file", written.file);
    if (request.operation != service_registry::Operation::Remove)
    {
        object.AddText("yaml", written.yaml);
    }

    object.AddJson("reload", ReloadCounts(written.report));
    return service_registry::Reply{service_registry::ReplyStatus::Ok, object.Text()};
}

service_registry::Reply Change(ProcessManagerParams& params, std::vector<Process>& retiring,
                               const service_registry::Request& request)
{
    const service_registry::Operation operation = request.operation;
    const std::string& name = request.name;
    const auto refuse = [operation, &name](const std::string& message, const std::vector<std::string>& problems)
    {
        return Refuse(operation, name, message, problems);
    };

    const service_registry::NameCode nameCode = service_registry::CheckName(name);
    if (nameCode != service_registry::NameCode::Ok)
    {
        return refuse(service_registry::DescribeName(nameCode, name), {});
    }

    if (ProcessConfigParams::IsReservedName(name))
    {
        return refuse(Quoted(name) + " is reserved: pm, all, BPM, PMC and GUI cannot name a service", {});
    }

    if (params.source.MainFile().empty())
    {
        return refuse("the manager's configuration was not read from a file, so it has no services.d", {});
    }

    const std::string mainFile = params.source.MainFile().string();
    const std::string folder = params.source.DropInFolder().string();
    config_source::LoadedConfig current{};
    std::string error;
    if (params.source.Load(current, error) != config_source::LoadCode::Ok)
    {
        return refuse("the configuration cannot be read: " + error + "; nothing was written", {});
    }

    if (current.folder == config_source::FolderCode::NotReadable || current.folder == config_source::FolderCode::NotFolder)
    {
        return refuse(folder + " cannot be listed; nothing was written", {});
    }

    if (!current.takesServices)
    {
        return refuse(mainFile + " has no Services list to add registered services to; fix it first", {});
    }

    const service_registry::ServiceFiles files{params.source.DropInFolder()};
    const std::string file = files.FileOf(name).string();
    const bool mainDefines = MainDefines(current, name);
    const bool registered = files.Exists(name);
    if (operation == service_registry::Operation::Add)
    {
        if (mainDefines)
        {
            return refuse("the main file " + mainFile + " defines " + Quoted(name) + "; edit it there", {});
        }

        if (registered)
        {
            return refuse(Quoted(name) + " is registered already (" + file + "); replace or update it, or remove it first",
                          {});
        }
    }
    else if (!registered)
    {
        return refuse(mainDefines ? Quoted(name) + " is defined in the main file " + mainFile +
                                        ", which only an edit of that file changes"
                                  : "no service named " + Quoted(name) + " is registered",
                      {});
    }
    else if (mainDefines && operation != service_registry::Operation::Remove)
    {
        return refuse("the main file " + mainFile + " defines " + Quoted(name) + ", and that definition is used instead of " +
                          file + "; edit the main file, or remove the file",
                      {});
    }

    config_section::ConfigValue definition{};
    if (operation == service_registry::Operation::Add || operation == service_registry::Operation::Replace)
    {
        definition = request.definition;
    }

    if (operation == service_registry::Operation::Update)
    {
        error.clear();
        const config_section::LoadCode read = config_section::LoadConfigFile(files.FileOf(name), definition, error);
        if (read != config_section::LoadCode::Ok || definition.kind != config_section::ValueKind::Mapping)
        {
            return refuse(file + " cannot be read as a service" + (error.empty() ? "" : ": " + error) +
                              "; replace it with a whole definition",
                          {});
        }

        if (service_registry::ApplyUpdate(definition, request.set, request.unset) != service_registry::EditCode::Ok)
        {
            return refuse("ProcessName cannot be changed: register the service under its new name and remove this one",
                          {});
        }
    }

    if (operation != service_registry::Operation::Remove)
    {
        const service_registry::EditCode named = service_registry::NameDefinition(definition, name);
        if (named == service_registry::EditCode::NameDiffers)
        {
            return refuse("the definition names another service in ProcessName; a registered service's ProcessName is "
                          "its name, " + Quoted(name),
                          {});
        }

        if (named != service_registry::EditCode::Ok)
        {
            return refuse("the definition must be a mapping of the service's keys to their values", {});
        }
    }

    std::vector<std::string> dependents;
    if (operation == service_registry::Operation::Remove && !mainDefines)
    {
        dependents = DependentsOf(current, name);
        if (!dependents.empty() && !request.force)
        {
            std::vector<std::string> needs;
            for (const std::string& dependent : dependents)
            {
                needs.push_back(dependent + " depends on " + Quoted(name));
            }

            return refuse(Quoted(name) + " is needed by other services; change them first, or remove it with force",
                          needs);
        }
    }

    // The configuration the change would make, checked the way validate checks one. Only what it adds counts.
    config_source::DropInOverride change{};
    change.serviceName = name;
    change.present = operation != service_registry::Operation::Remove;
    change.definition = definition;
    config_source::LoadedConfig candidate{};
    if (params.source.Load(candidate, error, {change}) != config_source::LoadCode::Ok)
    {
        return refuse("the configuration cannot be read: " + error + "; nothing was written", {});
    }

    std::vector<std::string> fresh = service_registry::NewProblems(ProblemsOf(current), ProblemsOf(candidate));
    for (const std::string& dependent : dependents)
    {
        // Removed with force: the dependents losing it is what was asked for.
        const std::string missing = "missing dependency: " + dependent + " depends on " + Quoted(name);
        fresh.erase(std::remove(fresh.begin(), fresh.end(), missing), fresh.end());
    }

    if (!fresh.empty())
    {
        return refuse("the change would bring " + std::to_string(fresh.size()) +
                          (fresh.size() == 1 ? " problem" : " problems") + "; nothing was written",
                      fresh);
    }

    Written written{};
    written.file = file;
    written.yaml = operation == service_registry::Operation::Remove ? std::string{} : service_registry::EmitYaml(definition);
    written.mainDefines = mainDefines;
    if (request.dryRun)
    {
        service_registry::JsonObject object = Header(operation);
        object.AddText("name", name);
        object.AddText("outcome", "valid");
        object.AddText("message", operation == service_registry::Operation::Remove
                                      ? Quoted(name) + " can be removed; nothing was written"
                                      : "the change is valid; nothing was written");
        object.AddText("file", file);
        if (operation != service_registry::Operation::Remove)
        {
            object.AddText("yaml", written.yaml);
        }

        return service_registry::Reply{service_registry::ReplyStatus::Ok, object.Text()};
    }

    service_registry::Snapshot snapshot{};
    if (files.Save(name, snapshot) != service_registry::StoreCode::Ok)
    {
        return refuse(file + " cannot be read to keep a copy of it; nothing was written", {});
    }

    const service_registry::StoreCode stored =
        operation == service_registry::Operation::Remove ? files.Remove(name) : files.Write(name, written.yaml);
    if (stored != service_registry::StoreCode::Ok)
    {
        files.Restore(snapshot);
        return refuse(file + (operation == service_registry::Operation::Remove ? " could not be removed"
                                                                              : " could not be written") +
                          "; nothing was changed",
                      {});
    }

    const Process* before = FindProcess(params, name);
    written.wasLoaded = before != nullptr;
    written.wasRunning = before != nullptr && before->HasPid();
    written.report = ProcessManager::Reload(params, retiring);
    if (written.report.result != ReloadResult::Applied)
    {
        const bool restored = files.Restore(snapshot) == service_registry::StoreCode::Ok;
        return refuse("the reload after the change was refused, because " + DescribeReload(written.report.result) +
                          (restored ? "; " + file + " is as it was" : "; " + file + " could not be put back"),
                      {});
    }

    return ReplyWritten(params, request, written);
}

} // namespace

service_registry::Reply ProcessManager::HandleRegistry(ProcessManagerParams& params, std::vector<Process>& retiring,
                                                       const std::vector<std::string>& frames)
{
    service_registry::Request request{};
    std::string problem;
    if (service_registry::ParseRequest(frames, request, problem) != service_registry::RequestCode::Ok)
    {
        logger::Warning("Registry: a request was not understood: %s", problem.c_str());
        service_registry::JsonObject object{};
        object.AddNumber("version", service_registry::protocol_version);
        object.AddText("message", problem);
        return service_registry::Reply{service_registry::ReplyStatus::Error, object.Text()};
    }

    switch (request.operation)
    {
    case service_registry::Operation::Version:
        return ReplyVersion(params);
    case service_registry::Operation::List:
        return ReplyList(params);
    case service_registry::Operation::Show:
        return ReplyShow(params, request);
    default:
        return Change(params, retiring, request);
    }
}

void ProcessManager::BindRegistry()
{
    const bool remote = processParams->remoteRegistration;
    const char* endpoint = remote ? comm::registry_port_remote : comm::registry_port_local;
    try
    {
        registrar.set(zmq::sockopt::linger, 0);
        registrar.bind(endpoint);
    }
    catch (const zmq::error_t& e)
    {
        logger::Error("Service registry could not bind %s: %s; services can still be registered in %s, then reload",
                      endpoint, e.what(), processParams->source.DropInFolder().c_str());
        return;
    }

    if (remote)
    {
        logger::Warning("Service registry listens on %s (RemoteRegistration: true): whoever reaches it can make the "
                        "manager run any program",
                        endpoint);
        return;
    }

    logger::Info("Service registry listens on %s, for this machine only", endpoint);
}

void ProcessManager::ServeRegistry()
{
    for (int i = 0; i < max_requests_per_cycle; ++i)
    {
        std::vector<zmq::message_t> frames;
        try
        {
            if (!zmq::recv_multipart(registrar, std::back_inserter(frames), zmq::recv_flags::dontwait))
            {
                return;
            }
        }
        catch (const zmq::error_t& e)
        {
            logger::Debug("Registry: %s", e.what());
            return;
        }

        if (frames.empty())
        {
            continue;
        }

        // The ROUTER puts the client's identity first; a REQ client adds an empty frame after it.
        const std::size_t envelopeSize = frames.size() > 1 && frames[1].size() == 0 ? 2 : 1;
        std::vector<std::string> request;
        for (std::size_t frame = envelopeSize; frame < frames.size(); ++frame)
        {
            request.push_back(frames[frame].to_string());
        }

        const service_registry::Reply reply = HandleRegistry(*processParams, retiring, request);
        const std::string status{service_registry::StatusText(reply.status)};
        try
        {
            for (std::size_t frame = 0; frame < envelopeSize; ++frame)
            {
                registrar.send(frames[frame], zmq::send_flags::sndmore);
            }

            registrar.send(zmq::buffer(status), zmq::send_flags::sndmore);
            registrar.send(zmq::buffer(reply.json), zmq::send_flags::none);
        }
        catch (const zmq::error_t& e)
        {
            logger::Error("Registry: the reply could not be sent: %s", e.what());
        }
    }
}

} // namespace process_manager
