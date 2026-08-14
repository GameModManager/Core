#include "engine/pipeline/plugin_host/diagnostics_registry.h"

#include "engine/core/log/logger.h"
#include "engine/game/plugins/plugin_database.h"

#include <cstring>

namespace engine {

DiagnosticsRegistry& DiagnosticsRegistry::instance() {
    static DiagnosticsRegistry registry;
    return registry;
}

void DiagnosticsRegistry::register_provider(const std::string& game_id,
                                            GmmDiagnosticsFn fn,
                                            void* user_data) {
    if (!fn) {
        Logger::instance().warn("Diagnostics provider registered with null fn");
        return;
    }
    providers_.push_back(Provider{game_id, fn, user_data});
    Logger::instance().debug("Diagnostics provider registered (game=" +
        (game_id.empty() ? std::string("any") : game_id) + ")");
}

void DiagnosticsRegistry::collect(const std::string& game_id,
                                  PluginDatabase& db) const {
    auto& ps = db.plugins_mutable();
    for (auto& p : ps) p.messages.clear();

    for (const auto& prov : providers_) {
        if (!prov.fn) continue;
        if (!prov.game_id.empty() && prov.game_id != game_id) continue;

        for (auto& p : ps) {
            char buffer[4096];
            std::memset(buffer, 0, sizeof(buffer));
            prov.fn(p.name.c_str(), buffer, sizeof(buffer), prov.user_data);

            // The provider writes zero or more NUL-terminated messages.
            size_t pos = 0;
            while (pos < sizeof(buffer)) {
                const char* msg = buffer + pos;
                if (*msg == '\0') break;
                const size_t len = std::strlen(msg);
                p.messages.emplace_back(msg, len);
                pos += len + 1;
            }
        }
    }
}

void DiagnosticsRegistry::clear() {
    providers_.clear();
}

}  // namespace engine
