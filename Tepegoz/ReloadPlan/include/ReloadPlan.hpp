#pragma once

#include "ConfigSection.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace config_source
{

struct LoadedConfig;

} // namespace config_source

namespace reload_plan
{

// Why a reload is refused as a whole. Anything but Ok leaves every service as it is.
enum class PlanCode
{
    Ok,
    // The drop-in folder exists but could not be listed: its files must not be taken for deleted.
    FolderNotReadable,
    // The main file's top level is not a mapping, or its Services is not a list.
    NoServicesList,
    // Two enabled services of the new configuration have the same ProcessName.
    DuplicateName,
};

// What a reload does to each service, by ProcessName. Every name is in one list at most.
struct ReloadPlan
{
    // In the new configuration and not in the manager: built, and started by its work mode as at start-up.
    std::vector<std::string> added;
    // In the manager, and in the new configuration neither by an entry nor by a file, or with Enabled: false.
    std::vector<std::string> removed;
    // In both, with another definition: the new one waits for the service's next start.
    std::vector<std::string> changed;
    // In the manager and still in the new configuration, but its new definition could not be read: kept as it is.
    std::vector<std::string> kept;
    // In both, with the same definition: left alone.
    std::vector<std::string> unchanged;
    // In the new configuration, not in the manager, and its definition could not be read: not added.
    std::vector<std::string> skipped;
    // The new configuration has an enabled entry whose ProcessName cannot be read. It could be any service, so none
    // is removed: those that would have been are kept instead.
    bool unnamedEntry = false;
};

/**
 * @brief Compare the services the manager has with a configuration read again, by ProcessName.
 * @param running The ProcessName of every service the manager has now, in its order.
 * @param before The document the manager's services were last built from.
 * @param after The configuration read again.
 * @param built The ProcessName of every service the manager could build from @p after's document, in start order.
 * @param out Receives the plan when it can be made.
 * @return PlanCode::Ok, otherwise why the reload must be refused.
 */
PlanCode PlanReload(const std::vector<std::string>& running, const config_section::ConfigValue& before,
                    const config_source::LoadedConfig& after, const std::vector<std::string>& built,
                    ReloadPlan& out);

// A pointer, not std::optional: the definition is read in place instead of copied.
/**
 * @brief Find the entry of an enabled service in a document's Services list.
 * @param document A configuration document.
 * @param processName The ProcessName to look for.
 * @return The first entry with that name that Enabled does not switch off, or nullptr when there is none.
 */
const config_section::ConfigValue* FindDefinition(const config_section::ConfigValue& document,
                                                  std::string_view processName);

/**
 * @brief Tell whether two parsed definitions are the same: the same keys with the same values.
 * @param a One definition.
 * @param b The other definition.
 * @return true when they are equal. Comments, key order and the layout of the file do not count.
 */
bool SameDefinition(const config_section::ConfigValue& a, const config_section::ConfigValue& b);

} // namespace reload_plan
