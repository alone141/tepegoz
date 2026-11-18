#include "ReloadPlan.hpp"

#include "ConfigSection.hpp"
#include "ConfigSource.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace reload_plan
{

namespace
{

constexpr std::string_view services_key = "Services";
constexpr std::string_view name_key = "ProcessName";
constexpr std::string_view enabled_key = "Enabled";

bool Contains(const std::vector<std::string>& names, std::string_view name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

void AddOnce(std::vector<std::string>& names, const std::string& name)
{
    if (!Contains(names, name))
    {
        names.push_back(name);
    }
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

// Read as the manager reads it: anything but a boolean leaves the entry on.
bool IsEnabled(const config_section::ConfigValue& entry)
{
    config_section::ConfigSection section{entry, "service"};
    return section.ReadBool(enabled_key, true);
}

// A pointer, not std::optional: the list is read in place instead of copied.
const config_section::ConfigValue* ServicesOf(const config_section::ConfigValue& document)
{
    const config_section::ConfigValue* services = config_section::FindValue(document, services_key);
    if (services == nullptr || services->kind != config_section::ValueKind::Sequence)
    {
        return nullptr;
    }

    return services;
}

// The services the new configuration has but whose definition could not be read: a drop-in file that was skipped
// for a problem of its own, or an enabled entry the manager could not build.
std::vector<std::string> UnreadableNames(const config_source::LoadedConfig& after, const std::vector<std::string>& built)
{
    std::vector<std::string> names;
    for (const config_source::DropInFile& file : after.dropIns)
    {
        // NameTaken: the main file defines the service, and its entry is what counts.
        if (file.code != config_source::DropInCode::Ok && file.code != config_source::DropInCode::NameTaken)
        {
            AddOnce(names, file.serviceName);
        }
    }

    const config_section::ConfigValue* services = ServicesOf(after.document);
    for (std::size_t i = 0; services != nullptr && i < services->children.size(); ++i)
    {
        const config_section::ConfigValue& entry = services->children[i];
        const std::string name = NameOf(entry);
        if (!name.empty() && IsEnabled(entry) && !Contains(built, name))
        {
            AddOnce(names, name);
        }
    }

    return names;
}

// Whether an entry, enabled or not, has this ProcessName: a switched-off service has one.
bool HasEntry(const config_section::ConfigValue& document, const std::string& name)
{
    const config_section::ConfigValue* services = ServicesOf(document);
    for (std::size_t i = 0; services != nullptr && i < services->children.size(); ++i)
    {
        if (NameOf(services->children[i]) == name)
        {
            return true;
        }
    }

    return false;
}

bool HasUnnamedEntry(const config_section::ConfigValue& document)
{
    const config_section::ConfigValue* services = ServicesOf(document);
    for (std::size_t i = 0; services != nullptr && i < services->children.size(); ++i)
    {
        const config_section::ConfigValue& entry = services->children[i];
        if (NameOf(entry).empty() && IsEnabled(entry))
        {
            return true;
        }
    }

    return false;
}

bool HasDuplicate(const std::vector<std::string>& names)
{
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        if (std::find(names.begin() + static_cast<std::ptrdiff_t>(i) + 1, names.end(), names[i]) != names.end())
        {
            return true;
        }
    }

    return false;
}

bool IsUnchanged(const config_section::ConfigValue& before, const config_section::ConfigValue& after,
                 const std::string& name)
{
    const config_section::ConfigValue* old = FindDefinition(before, name);
    const config_section::ConfigValue* now = FindDefinition(after, name);
    return old != nullptr && now != nullptr && SameDefinition(*old, *now);
}

} // namespace

PlanCode PlanReload(const std::vector<std::string>& running, const config_section::ConfigValue& before,
                    const config_source::LoadedConfig& after, const std::vector<std::string>& built,
                    ReloadPlan& out)
{
    if (after.folder == config_source::FolderCode::NotReadable || after.folder == config_source::FolderCode::NotFolder)
    {
        return PlanCode::FolderNotReadable;
    }

    if (!after.takesServices)
    {
        return PlanCode::NoServicesList;
    }

    if (HasDuplicate(built))
    {
        return PlanCode::DuplicateName;
    }

    const std::vector<std::string> unreadable = UnreadableNames(after, built);
    ReloadPlan plan{};
    plan.unnamedEntry = HasUnnamedEntry(after.document);
    std::vector<std::string> seen;
    for (const std::string& name : running)
    {
        if (Contains(seen, name))
        {
            continue;
        }

        seen.push_back(name);
        if (Contains(built, name))
        {
            std::vector<std::string>& list = IsUnchanged(before, after.document, name) ? plan.unchanged : plan.changed;
            list.push_back(name);
        }
        else if (Contains(unreadable, name) || (plan.unnamedEntry && !HasEntry(after.document, name)))
        {
            plan.kept.push_back(name);
        }
        else
        {
            plan.removed.push_back(name);
        }
    }

    for (const std::string& name : built)
    {
        if (!Contains(running, name))
        {
            plan.added.push_back(name);
        }
    }

    for (const std::string& name : unreadable)
    {
        if (!Contains(running, name) && !Contains(built, name))
        {
            plan.skipped.push_back(name);
        }
    }

    out = plan;
    return PlanCode::Ok;
}

const config_section::ConfigValue* FindDefinition(const config_section::ConfigValue& document,
                                                  std::string_view processName)
{
    const config_section::ConfigValue* services = ServicesOf(document);
    for (std::size_t i = 0; services != nullptr && i < services->children.size(); ++i)
    {
        const config_section::ConfigValue& entry = services->children[i];
        if (NameOf(entry) == processName && IsEnabled(entry))
        {
            return &entry;
        }
    }

    return nullptr;
}

bool SameDefinition(const config_section::ConfigValue& a, const config_section::ConfigValue& b)
{
    if (a.kind != b.kind || a.text != b.text || a.keys.size() != b.keys.size() ||
        a.children.size() != b.children.size())
    {
        return false;
    }

    if (a.kind != config_section::ValueKind::Mapping)
    {
        for (std::size_t i = 0; i < a.children.size(); ++i)
        {
            if (!SameDefinition(a.children[i], b.children[i]))
            {
                return false;
            }
        }

        return true;
    }

    // Matched by key, so the order the keys were read in does not count.
    for (std::size_t i = 0; i < a.keys.size(); ++i)
    {
        const config_section::ConfigValue* other = config_section::FindValue(b, a.keys[i]);
        if (other == nullptr || i >= a.children.size() || !SameDefinition(a.children[i], *other))
        {
            return false;
        }
    }

    return true;
}

} // namespace reload_plan
