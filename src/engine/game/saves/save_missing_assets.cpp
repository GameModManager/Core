#include "engine/game/saves/save_missing_assets.h"

#include "engine/core/vfs/path_resolver.h"
#include "engine/game/plugins/plugin_file.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>

namespace engine {

namespace {

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

// Scans one directory's top level for plugin files and records every hit whose
// name (case-insensitively) is a missing save plugin. Enumerates through
// PathResolver so the case-insensitive walk goes through the single canonical
// seam instead of a hand-rolled directory_iterator + to_lower.
void collect_providers(
    const vfs::PathResolver &resolver, std::string_view dir_rel,
    const std::string &mod_name,
    const std::map<std::string, SaveMissingAsset *> &missing) {
  for (const auto &gf : resolver.list(dir_rel)) {
    // Mirror the original hand-rolled scan: only real plugin files count,
    // never a directory whose name merely ends in a plugin extension.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(gf.absolute(), ec) || ec)
      continue;
    if (!is_plugin_file(gf.absolute()))
      continue;
    // gf.normalized() is the full CI key; its filename component is the
    // lowercased name, which matches the missing-map key (to_lower of the
    // plugin name).
    const std::string lower_name =
        std::filesystem::path(gf.normalized()).filename().string();
    const auto it = missing.find(lower_name);
    if (it == missing.end())
      continue;
    std::vector<std::string> &list = it->second->providing_mods;
    if (std::find(list.begin(), list.end(), mod_name) == list.end()) {
      list.push_back(mod_name);
    }
  }
}

} // namespace

std::vector<SaveMissingAsset>
find_save_missing_assets(const SaveGame &save,
                         const std::vector<GamePlugin> &plugins,
                         const std::filesystem::path &mods_dir,
                         const std::filesystem::path &overwrite_dir) {
  // Case-insensitive index of the current load order.
  std::map<std::string, const GamePlugin *> by_name;
  for (const auto &p : plugins) {
    by_name[to_lower(p.name)] = &p;
  }

  // Pass 1: decide which save plugins are missing (MO2 state machine).
  std::vector<SaveMissingAsset> missing;
  std::vector<std::string> missing_keys; // lowercased, parallel to `missing`
  const auto consider = [&](const std::string &name) {
    const auto it = by_name.find(to_lower(name));
    // Force-loaded (game-native/CC) plugins are always active in-game and
    // can never be missing, regardless of the snapshot's enabled flag.
    if (it != by_name.end() &&
        (it->second->enabled || it->second->force_loaded)) {
      return; // STATE_ACTIVE - the save's dependency is satisfied.
    }
    SaveMissingAsset asset;
    asset.plugin_name = name;
    if (it != by_name.end()) {
      asset.inactive = true; // STATE_INACTIVE - present but disabled.
      asset.origin_mod = it->second->owner_mod;
    } // else STATE_MISSING - absent from the load order entirely.
    missing.push_back(std::move(asset));
    missing_keys.push_back(to_lower(name));
  };
  for (const auto &name : save.plugins)
    consider(name);
  for (const auto &name : save.light_plugins)
    consider(name);

  if (missing.empty()) {
    return missing;
  }

  // Pointers into `missing` are only safe after pass 1 stops pushing.
  std::map<std::string, SaveMissingAsset *> missing_by_lower;
  for (std::size_t i = 0; i < missing.size(); ++i) {
    missing_by_lower[missing_keys[i]] = &missing[i];
  }

  // Pass 2: find candidate providers among installed mods (top level only,
  // matching MO2 entryList(espFilter) and our install layout).
  std::error_code ec;
  vfs::PathResolver mods_resolver(mods_dir);
  for (const auto &entry : std::filesystem::directory_iterator(mods_dir, ec)) {
    if (ec)
      break;
    if (!entry.is_directory(ec))
      continue;
    collect_providers(mods_resolver, entry.path().filename().string(),
                      entry.path().filename().string(), missing_by_lower);
  }

  if (!overwrite_dir.empty()) {
    vfs::PathResolver overwrite_resolver(overwrite_dir);
    collect_providers(overwrite_resolver, "", "<overwrite>", missing_by_lower);
  }

  return missing;
}

} // namespace engine
