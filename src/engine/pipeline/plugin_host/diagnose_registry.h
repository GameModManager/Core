#pragma once

#include <cstddef>
#include <string>
#include <vector>

// v2 ABI diagnostics callback signature + problem struct (gmm_abi_v2.h) -
// pure C, Qt-free.
#include "gmm_abi_v2.h"

namespace engine {

// Process-wide registry of v2 plugin diagnose providers (MO2 IPluginDiagnose
// parity). A v2 plugin registers a GmmDiagnoseFn via the ABI
// register_diagnostics callback; the engine stores it keyed by game_id and the
// registering plugin's path. collect_diagnostics(game_id) invokes every
// provider registered for that game and returns the aggregated
// GmmDiagnosticProblem structs for display (e.g. the Plugins-tab tooltip).
//
// Lifetime contract: a provider returns a heap array of GmmDiagnosticProblem
// that it owns; the engine copies each struct (shallow) into the returned
// vector and does NOT free the plugin's array. The const char* fields
// therefore point into plugin-owned memory and remain valid only while the
// plugin is loaded. Callers must consume the result before the plugin is
// unloaded.
class DiagnoseRegistry {
public:
  static DiagnoseRegistry &instance();

  // Register a diagnose provider. game_id scopes the provider to one game
  // ("" = all games); plugin_path is the .so path, used to drop the
  // provider on unload. fn must be non-null.
  void register_diagnostics(const std::string &game_id, GmmDiagnoseFn fn,
                            void *user_data, const std::string &plugin_path);

  // Invoke every provider matching game_id and return the aggregated
  // problems. Providers whose game_id is non-empty and != game_id are
  // skipped. Returns an empty vector when nothing matches.
  std::vector<GmmDiagnosticProblem>
  collect_diagnostics(const std::string &game_id) const;

  // Drop every provider registered by the given plugin path (called from
  // PluginLoader::unload_all before dlclose so no dangling fn pointer
  // survives).
  void clear_plugin(const std::string &plugin_path);

  // Drop all providers (process shutdown / full reload).
  void clear();

private:
  DiagnoseRegistry() = default;

  struct Entry {
    std::string game_id;
    GmmDiagnoseFn fn = nullptr;
    void *user_data = nullptr;
    std::string plugin_path;
  };

  std::vector<Entry> entries_;
};

} // namespace engine
