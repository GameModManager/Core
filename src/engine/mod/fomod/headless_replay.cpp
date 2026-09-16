#include "engine/mod/fomod/headless_replay.h"

#include "engine/core/log/logger.h"
#include "engine/mod/fomod/view_models.h"

#include <algorithm>
#include <string>

namespace engine {

namespace {

// Split "stepName/groupName" on the first '/'. Returns {stepName, groupName}
// or {"", ""} if no slash is present.
std::pair<std::string, std::string> parse_selection_key(const std::string& key)
{
    const auto pos = key.find('/');
    if (pos == std::string::npos) {
        return {key, {}};
    }
    return {key.substr(0, pos), key.substr(pos + 1)};
}

} // namespace

ReplayResult replay_fomod_choices(
    FomodViewModel& view_model,
    const Collection::InstallerChoices& choices)
{
    ReplayResult result;

    if (choices.type != "fomod") {
        Logger::instance().warn("replay_fomod_choices: unsupported installer type '" + choices.type + "'");
        result.success = false;
        return result;
    }

    const auto steps = view_model.getSteps();

    for (const auto& [key, pluginNames] : choices.selections) {
        const auto [stepName, groupName] = parse_selection_key(key);

        // Find the matching step by name.
        const auto stepIt = std::find_if(steps.begin(), steps.end(),
            [&stepName](const auto& step) { return step->getName() == stepName; });

        if (stepIt == steps.end()) {
            result.unrecognized_keys.push_back(key);
            Logger::instance().debug("replay_fomod_choices: no step named '" + stepName + "'");
            continue;
        }

        const auto& step = *stepIt;

        // Find the matching group within the step.
        const auto& groups = step->getGroups();
        const auto groupIt = std::find_if(groups.begin(), groups.end(),
            [&groupName](const auto& group) { return group->getName() == groupName; });

        if (groupIt == groups.end()) {
            result.unrecognized_keys.push_back(key);
            Logger::instance().debug(
                "replay_fomod_choices: no group named '" + groupName + "' in step '" + stepName + "'");
            continue;
        }

        const auto& group = *groupIt;

        // Select each named plugin in the group.
        for (const auto& pluginName : pluginNames) {
            const auto& plugins = group->getPlugins();
            const auto pluginIt = std::find_if(plugins.begin(), plugins.end(),
                [&pluginName](const auto& p) { return p->getName() == pluginName; });

            if (pluginIt == plugins.end()) {
                result.unrecognized_options.push_back(pluginName);
                Logger::instance().debug(
                    "replay_fomod_choices: no plugin named '" + pluginName
                    + "' in group '" + groupName + "' of step '" + stepName + "'");
                continue;
            }

            if (!(*pluginIt)->isSelected() && (*pluginIt)->isEnabled()) {
                view_model.togglePlugin(group, *pluginIt, true);
            }
        }
    }

    Logger::instance().info(
        "replay_fomod_choices: applied " + std::to_string(choices.selections.size())
        + " selection(s), " + std::to_string(result.unrecognized_keys.size()) + " unrecognized key(s), "
        + std::to_string(result.unrecognized_options.size()) + " unrecognized option(s)");

    return result;
}

ChoiceResolution resolve_choice_groups(
    const std::vector<Collection::ChoiceGroup>& groups,
    const std::unordered_map<std::string, std::vector<std::string>>& picks)
{
    ChoiceResolution result;

    for (const auto& [key, _] : picks) {
        const auto groupIt = std::find_if(groups.begin(), groups.end(),
            [&key](const auto& group) { return group.id == key; });
        if (groupIt == groups.end()) {
            result.unrecognized_groups.push_back(key);
            Logger::instance().debug("resolve_choice_groups: no group with id '" + key + "'");
        }
    }

    for (const auto& group : groups) {
        std::vector<std::string> valid;
        const auto pickIt = picks.find(group.id);
        if (pickIt != picks.end()) {
            for (const auto& modId : pickIt->second) {
                const bool isMember = std::find(group.member_mod_ids.begin(),
                    group.member_mod_ids.end(), modId) != group.member_mod_ids.end();
                if (isMember) {
                    valid.push_back(modId);
                } else {
                    result.unrecognized_members.push_back(modId);
                    Logger::instance().debug("resolve_choice_groups: '" + modId
                        + "' is not a member of group '" + group.id + "'");
                }
            }
        }

        const bool exactlyOne = group.mode == Collection::ChoiceMode::ExactlyOne;
        if ((exactlyOne && valid.size() != 1) || (!exactlyOne && valid.size() > 1)) {
            result.mode_violations.push_back(group.id);
            result.success = false;
        }
        if (!valid.empty()) {
            result.selected_mod_ids.push_back(valid.front());
        }
    }

    return result;
}

} // namespace engine
