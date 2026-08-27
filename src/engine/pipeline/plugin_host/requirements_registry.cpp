#include "engine/pipeline/plugin_host/requirements_registry.h"

#include "engine/core/log/logger.h"

namespace engine {

RequirementsRegistry& RequirementsRegistry::instance() {
    static RequirementsRegistry inst;
    return inst;
}

void RequirementsRegistry::register_requirements(const std::string& plugin_path,
                                                 GmmRequirementsFn fn,
                                                 void* user_data) {
    if (!fn) return;
    entries_.push_back({fn, user_data, plugin_path});
    Logger::instance().debug("Registered requirements provider (plugin=" +
        plugin_path + ")");
}

std::vector<UnmetRequirement> RequirementsRegistry::check_requirements(
    const std::vector<PluginInfo>& plugins) const {
    std::vector<UnmetRequirement> unmet;

    for (const auto& entry : entries_) {
        size_t count = 0;
        GmmPluginRequirement* reqs = entry.fn(&count, entry.user_data);
        if (!reqs) continue;

        for (size_t i = 0; i < count; ++i) {
            const GmmPluginRequirement& r = reqs[i];
            std::string type = r.type ? r.type : "";
            std::string name = r.name ? r.name : "";
            std::string message = r.message ? r.message : "";

            if (!is_requirement_met(type, name, plugins)) {
                UnmetRequirement u;
                u.plugin_path = entry.plugin_path;
                u.type = type;
                u.name = name;
                u.message = message;
                unmet.push_back(std::move(u));
            }
        }
        // The plugin owns the returned array; the engine only copied the
        // strings into the UnmetRequirement structs above.
    }

    return unmet;
}

void RequirementsRegistry::clear_plugin(const std::string& plugin_path) {
    for (size_t i = 0; i < entries_.size();) {
        if (entries_[i].plugin_path == plugin_path) {
            entries_.erase(entries_.begin() + i);
        } else {
            ++i;
        }
    }
}

void RequirementsRegistry::clear() {
    entries_.clear();
}

bool RequirementsRegistry::is_requirement_met(
    const std::string& type,
    const std::string& name,
    const std::vector<PluginInfo>& plugins) {
    if (name.empty()) {
        // No specific target named: nothing to verify, treat as satisfied.
        return true;
    }

    if (type == "plugin") {
        for (const auto& p : plugins) {
            if (p.plugin_name == name ||
                p.game_id == name ||
                p.game_display_name == name) {
                return true;
            }
        }
        return false;
    }

    if (type == "game") {
        for (const auto& p : plugins) {
            if (!p.game_support) continue;
            if (p.game_id == name || p.game_display_name == name) {
                return true;
            }
        }
        return false;
    }

    if (type == "diagnose") {
        for (const auto& p : plugins) {
            for (const auto& d : p.diagnostics_v2) {
                if (d.game_id == name || d.game_id.empty()) {
                    return true;
                }
            }
        }
        return false;
    }

    // Unknown requirement type: cannot evaluate, so do not flag as unmet.
    return true;
}

}  // namespace engine
