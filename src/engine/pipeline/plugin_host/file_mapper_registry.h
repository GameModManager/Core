#pragma once

#include <cstddef>
#include <string>
#include <vector>

// v2 ABI file-mapper callback signature + mapping struct (gmm_abi_v2.h) -
// pure C, Qt-free.
#include "gmm_abi_v2.h"

namespace engine {

// Process-wide registry of v2 plugin file mappers (MO2 IPluginFileMapper
// parity). A v2 plugin registers a GmmFileMapperFn via the ABI
// register_file_mapper callback; the engine stores it keyed by game_id and the
// registering plugin's path. get_mappings(game_id) invokes every mapper
// registered for that game and returns the aggregated {source, target} pairs
// that the deploy pipeline can turn into virtual file overlays.
//
// Lifetime contract: a mapper returns a heap array of GmmFileMapping that it
// owns; the engine copies each struct (shallow) into the returned vector and
// does NOT free the plugin's array. The const char* fields therefore point
// into plugin-owned memory and remain valid only while the plugin is loaded.
// Callers must consume the result before the plugin is unloaded.
class FileMapperRegistry {
public:
  static FileMapperRegistry &instance();

  // Register a file mapper. game_id scopes the mapper to one game
  // ("" = all games); plugin_path is the .so path, used to drop the
  // mapper on unload. fn must be non-null.
  void register_mapper(const std::string &game_id, GmmFileMapperFn fn,
                       void *user_data, const std::string &plugin_path);

  // Invoke every mapper matching game_id and return the aggregated
  // {source, target} pairs. Mappers whose game_id is non-empty and != game_id
  // are skipped. Returns an empty vector when nothing matches.
  std::vector<GmmFileMapping> get_mappings(const std::string &game_id) const;

  // Drop every mapper registered by the given plugin path (called from
  // PluginLoader::unload_all before dlclose so no dangling fn pointer
  // survives).
  void clear_plugin(const std::string &plugin_path);

  // Drop all mappers (process shutdown / full reload).
  void clear();

private:
  FileMapperRegistry() = default;

  struct Entry {
    std::string game_id;
    GmmFileMapperFn fn = nullptr;
    void *user_data = nullptr;
    std::string plugin_path;
  };

  std::vector<Entry> entries_;
};

} // namespace engine
