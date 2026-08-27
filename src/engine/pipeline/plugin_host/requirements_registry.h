#pragma once

#include <cstddef>
#include <string>
#include <vector>

// PluginInfo carries the loaded-plugin set used to evaluate whether a
// requirement is satisfied. Including the loader header here is safe: it does
// not (transitively) include this file, so there is no include cycle. It is
// included before gmm_abi_v2.h so its unconditional `#define GMM_ABI_VERSION 1`
// wins and the v2 header's guarded definition is skipped (avoids a redefinition
// warning, mirroring the include order in plugin_loader.cpp).
#include "engine/pipeline/plugin_host/plugin_loader.h"

// v2 ABI requirement callback signature + struct (gmm_abi_v2.h) - pure C,
// Qt-free.
#include "gmm_abi_v2.h"

namespace engine {

// One requirement a plugin declared that is NOT currently satisfied by the
// loaded plugin/game set. Returned by RequirementsRegistry::check_requirements
// so the loader can surface it (log a warning today; a UI banner later).
struct UnmetRequirement {
    std::string plugin_path;  // the plugin that declared the requirement
    std::string type;         // "plugin" | "game" | "diagnose"
    std::string name;         // required plugin/game name
    std::string message;      // human-readable message if not met
};

// Process-wide registry of v2 plugin requirement providers (MO2 IPlugin
// requirements parity). A v2 plugin registers a GmmRequirementsFn via the ABI
// register_requirements callback; the engine stores it keyed by the
// registering plugin's path. check_requirements() invokes every provider once
// all plugins are loaded and returns the aggregated list of requirements that
// are not yet satisfied by the loaded plugin/game set.
//
// Lifetime contract: a provider returns a heap array of GmmPluginRequirement
// that it owns; the engine copies each struct (shallow) into the result and
// does NOT free the plugin's array. The const char* fields therefore point
// into plugin-owned memory and remain valid only while the plugin is loaded.
class RequirementsRegistry {
public:
    static RequirementsRegistry& instance();

    // Register a requirement provider. fn must be non-null; plugin_path is the
    // .so path, used to drop the provider on unload.
    void register_requirements(const std::string& plugin_path,
                               GmmRequirementsFn fn,
                               void* user_data);

    // Invoke every registered provider and return the aggregated list of
    // requirements that are NOT satisfied by the currently loaded plugins
    // (the plugins_ set owned by the loader). A requirement is considered met
    // when a loaded plugin matches its name/type (see is_requirement_met).
    std::vector<UnmetRequirement> check_requirements(
        const std::vector<PluginInfo>& plugins) const;

    // Drop every provider registered by the given plugin path (called from
    // PluginLoader::unload_all before dlclose so no dangling fn pointer
    // survives).
    void clear_plugin(const std::string& plugin_path);

    // Drop all providers (process shutdown / full reload).
    void clear();

private:
    RequirementsRegistry() = default;

    struct Entry {
        GmmRequirementsFn fn = nullptr;
        void* user_data = nullptr;
        std::string plugin_path;
    };

    // Evaluate a single requirement against the loaded plugin set.
    static bool is_requirement_met(const std::string& type,
                                   const std::string& name,
                                   const std::vector<PluginInfo>& plugins);

    std::vector<Entry> entries_;
};

}  // namespace engine
