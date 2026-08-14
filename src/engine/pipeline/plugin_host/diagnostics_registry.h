#pragma once

#include <cstddef>
#include <string>
#include <vector>

// ABI diagnostics callback signature (gmm_abi_v1.h) - pure C, Qt-free.
#include "gmm_abi_v1.h"

namespace engine {

class PluginDatabase;

// Per-game plugin diagnostics providers (MO2 addInformation parity). C
// plugins register a GmmDiagnosticsFn via the ABI register_diagnostics;
// the engine calls each provider once per plugin after the plugin database
// refreshes and appends the returned messages to GamePlugin::messages,
// which the Plugins tab tooltip renders below a <hr>.
class DiagnosticsRegistry {
public:
    static DiagnosticsRegistry& instance();

    // game_id: plugin game this provider serves ("" = all games).
    // fn: called with (plugin_name, buffer, capacity, user_data); the
    //     provider writes zero or more NUL-terminated messages.
    void register_provider(const std::string& game_id,
                           GmmDiagnosticsFn fn,
                           void* user_data);

    // Re-run every matching provider over all plugins in db, replacing each
    // plugin's messages. No-op when no provider matches game_id.
    void collect(const std::string& game_id, PluginDatabase& db) const;

    // Drop all providers (Python shutdown path; acquire the GIL first).
    void clear();

private:
    DiagnosticsRegistry() = default;

    struct Provider {
        std::string game_id;
        GmmDiagnosticsFn fn = nullptr;
        void* user_data = nullptr;
    };

    std::vector<Provider> providers_;
};

}  // namespace engine
