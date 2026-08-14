#pragma once

// Ported from FOMOD Plus (MIT), installer/lib/ViewModels.h and
// installer/lib/FlagMap.h. Qt-free.

#include "engine/mod/fomod/module_config.h"
#include "engine/mod/fomod/fomod_utils.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {

class FomodViewModel;

template <typename T> using shared_ptr_list = std::vector<std::shared_ptr<T>>;

/*
--------------------------------------------------------------------------------
                                Plugins
--------------------------------------------------------------------------------
*/
class PluginViewModel {
public:
    PluginViewModel(const std::shared_ptr<Plugin>& plugin_, const bool selected, bool /*enabled*/,
        const int index)
        : ownIndex(index)
        , selected(selected)
        , enabled(true)
        , manuallySet(false)
        , plugin(plugin_)
    {
    }

    void setSelected(const bool selected) { this->selected = selected; }
    void setEnabled(const bool enabled) { this->enabled = enabled; }
    [[nodiscard]] std::string getName() const { return plugin ? plugin->name : std::string(); }
    [[nodiscard]] std::string getDescription() const { return plugin ? plugin->description : std::string(); }
    [[nodiscard]] std::string getImagePath() const { return plugin ? plugin->image.path : std::string(); }
    [[nodiscard]] bool isSelected() const { return selected; }
    [[nodiscard]] bool isEnabled() const { return enabled; }
    [[nodiscard]] int getOwnIndex() const { return ownIndex; }
    [[nodiscard]] std::vector<ConditionFlag> getConditionFlags() const { return plugin->conditionFlags.flags; }
    [[nodiscard]] PluginTypeEnum getCurrentPluginType() const { return currentPluginType; }
    [[nodiscard]] bool wasManuallySet() const { return manuallySet; }

    void setCurrentPluginType(const PluginTypeEnum type) { currentPluginType = type; }
    void setStepIndex(const int stepIndex) { this->stepIndex = stepIndex; }
    void setGroupIndex(const int groupIndex) { this->groupIndex = groupIndex; }

    [[nodiscard]] int getStepIndex() const { return stepIndex; }
    [[nodiscard]] int getGroupIndex() const { return groupIndex; }

    friend class FomodViewModel;
    friend class FomodFileInstaller;
    friend class FomodConditionTester;
    friend std::vector<File> collect_files_to_install(const FomodViewModel&);

protected:
    [[nodiscard]] std::shared_ptr<Plugin> getPlugin() const { return plugin; }

private:
    int ownIndex;
    bool selected;
    bool enabled;
    bool manuallySet;
    PluginTypeEnum currentPluginType = PluginTypeEnum::UNKNOWN;
    std::shared_ptr<Plugin> plugin;

    int stepIndex = -1;
    int groupIndex = -1;
};

/*
--------------------------------------------------------------------------------
                                Groups
--------------------------------------------------------------------------------
*/
class GroupViewModel {
public:
    GroupViewModel(const std::shared_ptr<Group>& group_, const shared_ptr_list<PluginViewModel>& plugins,
        const int index, const int stepIndex)
        : plugins(plugins)
        , group(group_)
        , ownIndex(index)
        , stepIndex(stepIndex)
    {
    }

    void addPlugin(const std::shared_ptr<PluginViewModel>& plugin) { plugins.emplace_back(plugin); }

    [[nodiscard]] std::string getName() const { return group->name; }
    [[nodiscard]] GroupTypeEnum getType() const { return group->type; }
    [[nodiscard]] const shared_ptr_list<PluginViewModel>& getPlugins() const { return plugins; }
    [[nodiscard]] int getOwnIndex() const { return ownIndex; }
    [[nodiscard]] int getStepIndex() const { return stepIndex; }

private:
    shared_ptr_list<PluginViewModel> plugins;
    std::shared_ptr<Group> group;
    int ownIndex;
    int stepIndex;
};

/*
--------------------------------------------------------------------------------
                                Steps
--------------------------------------------------------------------------------
*/
class StepViewModel {
public:
    StepViewModel(const std::shared_ptr<InstallStep>& installStep_,
        const shared_ptr_list<GroupViewModel>& groups, const int index)
        : installStep(installStep_)
        , groups(groups)
        , ownIndex(index)
    {
    }

    [[nodiscard]] CompositeDependency& getVisibilityConditions() const { return installStep->visible; }
    [[nodiscard]] std::string getName() const { return installStep->name; }
    [[nodiscard]] const shared_ptr_list<GroupViewModel>& getGroups() const { return groups; }
    [[nodiscard]] int getOwnIndex() const { return ownIndex; }
    [[nodiscard]] bool getHasVisited() const { return visited; }
    void setVisited(const bool visited) { this->visited = visited; }

private:
    bool visited = false;
    std::shared_ptr<InstallStep> installStep;
    shared_ptr_list<GroupViewModel> groups;
    int ownIndex;
};

/*
--------------------------------------------------------------------------------
                            Outbound Types
--------------------------------------------------------------------------------
*/
using StepRef = const std::shared_ptr<StepViewModel>&;
using GroupRef = const std::shared_ptr<GroupViewModel>&;
using PluginRef = const std::shared_ptr<PluginViewModel>&;

using Flag = std::pair<std::string, std::string>;
using FlagList = std::vector<Flag>;

// A map from plugin → the condition flags it sets while selected. Ordering of
// getFlagsByKey is deliberate: step descending, then plugin ascending, so a
// flag set by a later step wins over an earlier one (FOMOD Plus FlagMap.h).
class FlagMap {
public:
    [[nodiscard]] std::vector<std::shared_ptr<PluginViewModel>> getPluginsSettingFlag(
        const std::string& key, const std::string& value) const
    {
        std::vector<std::shared_ptr<PluginViewModel>> result;
        for (const auto& [plugin, theseFlags] : flags) {
            for (const auto& [fst, snd] : theseFlags) {
                if (toLower(fst) == toLower(key) && snd == value) {
                    result.emplace_back(plugin);
                }
            }
        }
        return result;
    }

    [[nodiscard]] FlagList getFlagsByKey(const std::string& key) const
    {
        FlagList result;
        std::vector<std::pair<int, std::shared_ptr<PluginViewModel>>> orderedPlugins;

        // Collect all plugins with their stepIndex and ownIndex
        for (const auto& [plugin, _] : flags) {
            orderedPlugins.emplace_back(plugin->getStepIndex(), plugin);
        }

        // Sort plugins by stepIndex and ownIndex, stepIndex descending and ownIndex ascending
        std::sort(orderedPlugins.begin(), orderedPlugins.end(),
            [](const auto& a, const auto& b) {
                return a.first > b.first || (a.first == b.first && a.second->getOwnIndex() < b.second->getOwnIndex());
            });

        // Collect flags in the sorted order
        for (const auto& [_, plugin] : orderedPlugins) {
            for (const auto& flag : flags.at(plugin)) {
                if (toLower(flag.first) == toLower(key)) {
                    result.emplace_back(flag);
                }
            }
        }
        return result;
    }

    void setFlagsForPlugin(PluginRef plugin)
    {
        // Don't clutter the map with empty key-vals
        if (plugin->getConditionFlags().empty()) {
            return;
        }
        unsetFlagsForPlugin(plugin);

        FlagList flagList;
        for (const auto& conditionFlag : plugin->getConditionFlags()) {
            flagList.emplace_back(toLower(conditionFlag.name), conditionFlag.value);
        }
        flags[plugin] = flagList;
    }

    void unsetFlagsForPlugin(PluginRef plugin)
    {
        if (flags.contains(plugin)) {
            flags.erase(plugin);
        }
    }

    void clearAll() { flags.clear(); }

    [[nodiscard]] size_t getFlagCount() const { return flags.size(); }

private:
    std::unordered_map<std::shared_ptr<PluginViewModel>, FlagList> flags;
};

}  // namespace engine
