#include "engine/mod/fomod/condition_tester.h"

#include "engine/mod/fomod/fomod_utils.h"
#include "engine/core/log/logger.h"

#include <algorithm>
#include <set>

namespace engine {

static std::string setToString(const std::set<int>& set)
{
    std::string str;
    for (const auto& i : set) {
        str += std::to_string(i) + ", ";
    }
    return str;
}

bool FomodConditionTester::isStepVisible(const std::shared_ptr<FlagMap>& flags,
    const CompositeDependency& compositeDependency, const int stepIndex,
    const std::vector<std::shared_ptr<StepViewModel>>& steps) const
{
    // first things first: is it visible?
    if (!testCompositeDependency(flags, compositeDependency)) {
        return false;
    }

    const auto flagDependencies = compositeDependency.flagDependencies;
    if (flagDependencies.empty()) {
        return true;
    }

    std::set<int> stepsThatSetThisFlag;

    for (const auto& flagDependency : flagDependencies) {
        // for this flag, find the plugins that set it
        for (int i = stepIndex - 1; i >= 0; --i) {
            for (const auto& group : steps[i]->getGroups()) {
                for (const auto& plugin : group->getPlugins()) {
                    if (std::any_of(plugin->getPlugin()->conditionFlags.flags.begin(),
                            plugin->getPlugin()->conditionFlags.flags.end(),
                            [&flagDependency](const ConditionFlag& flag) {
                                return flag.name == flagDependency.flag && flag.value == flagDependency.value;
                            })) {
                        stepsThatSetThisFlag.insert(i);
                    }
                }
            }
        }
    }
    const auto anyVisible = std::any_of(stepsThatSetThisFlag.begin(), stepsThatSetThisFlag.end(),
        [this, &steps, &flags](const int index) {
            return isStepVisible(flags, steps[index]->getVisibilityConditions(), index, steps);
        });
    if (!anyVisible) {
        Logger::instance().debug("Step " + steps[stepIndex]->getName() + " has no dependent steps that are visible.");
        Logger::instance().debug("Steps that set this flag: " + setToString(stepsThatSetThisFlag));
    }
    return anyVisible;
}

bool FomodConditionTester::testCompositeDependency(
    const std::shared_ptr<FlagMap>& flags, const CompositeDependency& compositeDependency) const
{
    const auto fileDependencies = compositeDependency.fileDependencies;
    const auto flagDependencies = compositeDependency.flagDependencies;
    const auto gameDependencies = compositeDependency.gameDependencies;
    const auto nestedDependencies = compositeDependency.nestedDependencies;
    const auto globalOperatorType = compositeDependency.operatorType;

    // For the globalOperatorType: evaluate all conditions and store the
    // results, then combine. Deliberately NOT short-circuiting (matches MO2).
    std::vector<bool> results;
    for (const auto& fileDependency : fileDependencies) {
        results.emplace_back(testFileDependency(fileDependency));
    }
    for (const auto& flagDependency : flagDependencies) {
        results.emplace_back(testFlagDependency(flags, flagDependency));
    }
    for (const auto& gameDependency : gameDependencies) {
        results.emplace_back(testGameDependency(gameDependency));
    }
    for (const auto& nestedDependency : nestedDependencies) {
        results.emplace_back(testCompositeDependency(flags, nestedDependency));
    }

    if (globalOperatorType == OperatorTypeEnum::AND) {
        return std::all_of(results.begin(), results.end(), [](const bool result) { return result; });
    }
    return std::any_of(results.begin(), results.end(), [](const bool result) { return result; });
}

bool FomodConditionTester::testFlagDependency(
    const std::shared_ptr<FlagMap>& flags, const FlagDependency& flagDependency)
{
    // Every instance of this flag being set in the map.
    const auto flagList = flags->getFlagsByKey(flagDependency.flag);

    // Find the first instance of this flag being set (in the order specified
    // by getFlagsByKey).
    if (flagList.empty()) {
        // If the dependency value is an empty string, it means this flag should be unset.
        // So if we don't have any value for this flag, the result is true.
        return flagDependency.value.empty();
    }

    return flagList.front().second == flagDependency.value;
}

bool FomodConditionTester::testFileDependency(const FileDependency& fileDependency) const
{
    const std::string& pluginName = fileDependency.file;
    const auto pluginState = getFileDependencyState(pluginName);
    return pluginState == fileDependency.state;
}

bool FomodConditionTester::testGameDependency(const GameDependency& gameDependency) const
{
    if (mGameVersionProvider == nullptr) {
        Logger::instance().warn("FomodConditionTester: no game-version provider; assuming version condition '"
                                + gameDependency.version + "' is satisfied");
        return true;
    }
    const auto gameVersion = mGameVersionProvider->game_version();
    Logger::instance().debug("Comparing condition version " + gameDependency.version + " against " + gameVersion);
    if (gameDependency.version <= gameVersion) {
        Logger::instance().debug("Version matches!");
    }
    return gameDependency.version <= gameVersion;
}

FileDependencyTypeEnum FomodConditionTester::getFileDependencyState(const std::string& fileName) const
{
    if (const auto it = fileDependencyCache.find(fileName); it != fileDependencyCache.end()) {
        return it->second;
    }

    FileDependencyTypeEnum state = FileDependencyTypeEnum::Missing;
    if (mFileStateResolver != nullptr) {
        state = mFileStateResolver->file_state(fileName);
    } else {
        Logger::instance().warn("FomodConditionTester: no file-state resolver; assuming file '" + fileName
                                + "' is missing");
    }

    fileDependencyCache[fileName] = state;
    return state;
}

PluginTypeEnum FomodConditionTester::getPluginTypeDescriptorState(
    const std::shared_ptr<Plugin>& plugin, const std::shared_ptr<FlagMap>& flags) const
{
    // A plugin's ConditionFlags aren't the same thing as a step visibility
    // one. A plugin's ConditionFlags are toggled based on the selection state
    // of the plugin. We only evaluate the typeDescriptor here.

    // We will return the 'winning' type or the default. If multiple conditions
    // are met, the first matching pattern wins (FOMOD Plus semantics).
    const auto& dependencyType = plugin->typeDescriptor.dependencyType;
    for (const auto& pattern : dependencyType.patterns.patterns) {
        if (testCompositeDependency(flags, pattern.dependencies)) {
            return pattern.type;
        }
    }

    // Sometimes authors do this.
    if (plugin->typeDescriptor.type != PluginTypeEnum::Optional) {
        return plugin->typeDescriptor.type;
    }
    if (plugin->typeDescriptor.dependencyType.defaultType.has_value()) {
        return plugin->typeDescriptor.dependencyType.defaultType.value();
    }
    return PluginTypeEnum::Optional;
}

}  // namespace engine
