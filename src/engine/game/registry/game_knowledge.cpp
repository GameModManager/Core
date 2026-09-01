#include "engine/game/registry/game_knowledge.h"
#include "platform/platform.h"

#include <cstdlib>

namespace engine {

void GameKnowledge::set(const std::string &game_id, const std::string &key,
                        const std::string &value) {
  data_[game_id][key] = value;
}

std::string GameKnowledge::get(const std::string &game_id,
                               const std::string &key,
                               const std::string &fallback) const {
  auto game_it = data_.find(game_id);
  if (game_it == data_.end())
    return fallback;

  auto key_it = game_it->second.find(key);
  if (key_it == game_it->second.end())
    return fallback;

  return key_it->second;
}

bool GameKnowledge::has(const std::string &game_id,
                        const std::string &key) const {
  auto game_it = data_.find(game_id);
  if (game_it == data_.end())
    return false;
  return game_it->second.count(key) > 0;
}

std::vector<std::string>
GameKnowledge::keys_for(const std::string &game_id) const {
  auto game_it = data_.find(game_id);
  if (game_it == data_.end())
    return {};

  std::vector<std::string> result;
  result.reserve(game_it->second.size());
  for (const auto &[key, _] : game_it->second) {
    result.push_back(key);
  }
  return result;
}

std::vector<std::string> GameKnowledge::registered_games() const {
  std::vector<std::string> result;
  result.reserve(data_.size());
  for (const auto &[game_id, _] : data_) {
    result.push_back(game_id);
  }
  return result;
}

void GameKnowledge::clear() { data_.clear(); }

std::string disable_mechanism_for(const GameKnowledge &knowledge,
                                  const std::string &game_id) {
  const std::string declared = knowledge.get(game_id, "disable_mechanism", "");
  if (!declared.empty())
    return declared;
  return kDefaultDisableMechanism;
}

std::string deploy_strategy_for(const GameKnowledge &knowledge,
                                const std::string &game_id) {
  const std::string declared = knowledge.get(game_id, "deploy_strategy", "");
  if (!declared.empty())
    return declared;
  return kDefaultDeployStrategy;
}

std::string creation_club_file_for(const GameKnowledge &knowledge,
                                   const std::string &game_id) {
  return knowledge.get(game_id, "creation_club_file", "skyrim.ccc");
}

bool delayed_disable_for(const GameKnowledge &knowledge,
                         const std::string &game_id) {
  return knowledge.get(game_id, "delayed_disable", "") == "true";
}
std::string plugin_game_mods_dir(const GameKnowledge &knowledge,
                                 const std::string &game_id) {
  std::string dir = knowledge.get(game_id, "game_mods_dir", "");
  // Expand a leading ~ against $HOME at resolution time (the plugin only
  // declares the literal path; HOME may differ between registration and use).
  if (!dir.empty() && dir.front() == '~') {
    dir = safe_home_dir().string() + dir.substr(1);
  }
  return dir;
}

std::filesystem::path
resolve_plugin_game_mods_dir(const std::string &game_id,
                             const std::filesystem::path &game_dir,
                             const GameKnowledge &knowledge) {
  const std::string declared = plugin_game_mods_dir(knowledge, game_id);
  if (declared.empty())
    return {};
  std::filesystem::path p(declared);
  // plugin_game_mods_dir has already ~-expanded the string for us; a
  // relative declaration (e.g. Isaac on Linux/Windows "mods") is anchored
  // to game_dir so the returned path is always absolute or empty.
  if (p.is_absolute())
    return p;
  if (game_dir.empty())
    return {};
  return game_dir / p;
}

std::filesystem::path resolve_game_mods_dir(
    const std::string &game_id, const std::filesystem::path &game_dir,
    const GameKnowledge &knowledge, const std::string &override_dir) {
  // 1. Per-instance user override (instance.toml "game_mods_dir") wins.
  if (!override_dir.empty())
    return std::filesystem::path(override_dir);
  // 2. Plugin-declared hook: absolute (Isaac on macOS) or relative
  //    (Isaac on Linux/Windows "mods" -> game_dir/mods). Relative values
  //    are resolved against game_dir via resolve_plugin_game_mods_dir.
  const auto declared =
      resolve_plugin_game_mods_dir(game_id, game_dir, knowledge);
  if (!declared.empty())
    return declared;
  // 3. mod_scan_subpath: a plugin-declared subdir of the game install that is
  //    genuinely a mods-only staging folder (rare). Empty = no game-dir
  //    scan source; the scanner uses the instance mods dir instead.
  //    Note: mods_subpath is intentionally NOT consulted here - it is a
  //    deploy target, not a scan source. Falling back to it (or to game_dir
  //    itself when both are empty) would walk vanilla game content
  //    (Data/, SKSE, Scripts, Meshes, Source, ...) and synthesize it as
  //    mods (MO2 only ever reads mods from <profile>/mods/).
  const std::string scan_subpath =
      knowledge.get(game_id, "mod_scan_subpath", "");
  if (!scan_subpath.empty())
    return game_dir / scan_subpath;
  // No game-dir mods source: the caller's mods dir (or the instance mods
  // dir) is the only legitimate scan target. Return an empty path so any
  // folder-level game-dir scan is suppressed; per-file stray synthesis
  // (unmanaged esp/esm/esl) runs against game_dir directly when needed.
  return {};
}

} // namespace engine
