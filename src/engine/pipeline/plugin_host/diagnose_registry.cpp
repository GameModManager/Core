#include "engine/pipeline/plugin_host/diagnose_registry.h"

#include "engine/core/log/logger.h"

#include <algorithm>

namespace engine {

DiagnoseRegistry &DiagnoseRegistry::instance() {
  static DiagnoseRegistry registry;
  return registry;
}

void DiagnoseRegistry::register_diagnostics(const std::string &game_id,
                                            GmmDiagnoseFn fn, void *user_data,
                                            const std::string &plugin_path) {
  if (!fn) {
    Logger::instance().warn("Diagnose provider registered with null fn");
    return;
  }
  entries_.push_back(Entry{game_id, fn, user_data, plugin_path});
  Logger::instance().debug("Diagnose provider registered (game=" +
                           (game_id.empty() ? std::string("any") : game_id) +
                           ", plugin=" + plugin_path + ")");
}

std::vector<GmmDiagnosticProblem>
DiagnoseRegistry::collect_diagnostics(const std::string &game_id) const {
  std::vector<GmmDiagnosticProblem> out;
  for (const auto &e : entries_) {
    if (!e.fn)
      continue;
    if (!e.game_id.empty() && e.game_id != game_id)
      continue;

    size_t count = 0;
    GmmDiagnosticProblem *problems = e.fn(&count, e.user_data);
    if (!problems)
      continue;

    for (size_t i = 0; i < count; ++i) {
      // Shallow copy: the const char* fields point into plugin-owned
      // memory (see class contract). The plugin retains ownership of the
      // returned array, so the engine must not free it.
      out.push_back(problems[i]);
    }
  }
  return out;
}

void DiagnoseRegistry::clear_plugin(const std::string &plugin_path) {
  auto it =
      std::remove_if(entries_.begin(), entries_.end(), [&](const Entry &e) {
        return e.plugin_path == plugin_path;
      });
  entries_.erase(it, entries_.end());
}

void DiagnoseRegistry::clear() { entries_.clear(); }

} // namespace engine
