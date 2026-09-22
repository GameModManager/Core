#include "engine/game/registry/game_knowledge.h"
#include "platform/platform.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace engine {

void GameKnowledge::set(const std::string& game_id, const std::string& key,
                        const std::string& value) {
  data_[game_id][key] = value;
}

std::string GameKnowledge::get(const std::string& game_id, const std::string& key,
                               const std::string& fallback) const {
  auto game_it = data_.find(game_id);
  if (game_it == data_.end())
    return fallback;

  auto key_it = game_it->second.find(key);
  if (key_it == game_it->second.end())
    return fallback;

  return key_it->second;
}

bool GameKnowledge::has(const std::string& game_id, const std::string& key) const {
  auto game_it = data_.find(game_id);
  if (game_it == data_.end())
    return false;
  return game_it->second.count(key) > 0;
}

std::vector<std::string> GameKnowledge::keys_for(const std::string& game_id) const {
  auto game_it = data_.find(game_id);
  if (game_it == data_.end())
    return {};

  std::vector<std::string> result;
  result.reserve(game_it->second.size());
  for (const auto& [key, _] : game_it->second) {
    result.push_back(key);
  }
  return result;
}

std::vector<std::string> GameKnowledge::registered_games() const {
  std::vector<std::string> result;
  result.reserve(data_.size());
  for (const auto& [game_id, _] : data_) {
    result.push_back(game_id);
  }
  return result;
}

void GameKnowledge::clear() {
  data_.clear();
}

std::string disable_mechanism_for(const GameKnowledge& knowledge,
                                  const std::string& game_id) {
  const std::string declared = knowledge.get(game_id, "disable_mechanism", "");
  if (!declared.empty())
    return declared;
  return kDefaultDisableMechanism;
}

std::string deploy_strategy_for(const GameKnowledge& knowledge,
                                const std::string& game_id) {
  const std::string declared = knowledge.get(game_id, "deploy_strategy", "");
  if (!declared.empty())
    return declared;
  return kDefaultDeployStrategy;
}

std::string creation_club_file_for(const GameKnowledge& knowledge,
                                   const std::string& game_id) {
  return knowledge.get(game_id, "creation_club_file", "skyrim.ccc");
}

bool delayed_disable_for(const GameKnowledge& knowledge, const std::string& game_id) {
  return knowledge.get(game_id, "delayed_disable", "") == "true";
}
std::string plugin_game_mods_dir(const GameKnowledge& knowledge,
                                 const std::string& game_id) {
  std::string dir = knowledge.get(game_id, "game_mods_dir", "");
  // Expand a leading ~ against $HOME at resolution time (the plugin only
  // declares the literal path; HOME may differ between registration and use).
  if (!dir.empty() && dir.front() == '~') {
    dir = safe_home_dir().string() + dir.substr(1);
  }
  return dir;
}

std::filesystem::path
resolve_plugin_game_mods_dir(const std::string& game_id,
                             const std::filesystem::path& game_dir,
                             const GameKnowledge& knowledge) {
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

std::filesystem::path resolve_game_mods_dir(const std::string& game_id,
                                            const std::filesystem::path& game_dir,
                                            const GameKnowledge& knowledge,
                                            const std::string& override_dir) {
  // 1. Per-instance user override (instance.toml "game_mods_dir") wins.
  if (!override_dir.empty())
    return std::filesystem::path(override_dir);
  // 2. Plugin-declared hook: absolute (Isaac on macOS) or relative
  //    (Isaac on Linux/Windows "mods" -> game_dir/mods). Relative values
  //    are resolved against game_dir via resolve_plugin_game_mods_dir.
  const auto declared = resolve_plugin_game_mods_dir(game_id, game_dir, knowledge);
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
  const std::string scan_subpath = knowledge.get(game_id, "mod_scan_subpath", "");
  if (!scan_subpath.empty())
    return game_dir / scan_subpath;
  // No game-dir mods source: the caller's mods dir (or the instance mods
  // dir) is the only legitimate scan target. Return an empty path so any
  // folder-level game-dir scan is suppressed; per-file stray synthesis
  // (unmanaged esp/esm/esl) runs against game_dir directly when needed.
  return {};
}

std::string mygames_leaf_for(const GameKnowledge& knowledge, const std::string& game_id,
                             const std::string& os_tag) {
  if (!os_tag.empty()) {
    const std::string per_os = knowledge.get(game_id, "mygames_folder_" + os_tag, "");
    if (!per_os.empty())
      return per_os;
  }
  return knowledge.get(game_id, "mygames_folder", "");
}

std::string mygames_parent_for(const GameKnowledge& knowledge,
                               const std::string& game_id) {
  const std::string parent = knowledge.get(game_id, "mygames_parent", "");
  if (!parent.empty())
    return parent;
  return "My Games";
}

namespace {

  uint32_t knowledge_appid(const GameKnowledge& knowledge, const std::string& game_id) {
    const std::string id_str = knowledge.get(game_id, "steam_appid", "");
    if (id_str.empty())
      return 0;
    try {
      return static_cast<uint32_t>(std::stoul(id_str));
    } catch (...) {
      return 0;
    }
  }

}  // namespace

std::filesystem::path resolve_mygames_dir(const std::string& game_id,
                                          const GameKnowledge& knowledge,
                                          const Platform* platform,
                                          bool is_windows_exe) {
  if (platform == nullptr || game_id.empty())
    return {};
  // A Windows executable on a non-Windows host runs under Proton: its
  // Documents folder lives inside the Steam prefix. Anything else (a native
  // game, or anything on native Windows) uses the host Documents folder.
  const bool under_proton = is_windows_exe && platform->platform_name() != "windows";
  if (!under_proton) {
    const std::string leaf =
        mygames_leaf_for(knowledge, game_id, platform->platform_name());
    if (leaf.empty())
      return {};
    const auto base = platform->native_documents_dir();
    if (base.empty())
      return {};
    return base / leaf;
  }
  const std::string leaf = knowledge.get(game_id, "mygames_folder", "");
  if (leaf.empty())
    return {};
  const uint32_t appid = knowledge_appid(knowledge, game_id);
  if (appid == 0)
    return {};
  const auto documents = platform->game_documents_dir(appid);
  if (documents.empty())
    return {};
  return documents / mygames_parent_for(knowledge, game_id) / leaf;
}

std::filesystem::path resolve_steam_userdata_saves_dir(const std::string& game_id,
                                                       const GameKnowledge& knowledge,
                                                       const Platform* platform) {
  if (platform == nullptr || game_id.empty())
    return {};
  const std::string subpath = knowledge.get(game_id, "steam_userdata_saves", "");
  if (subpath.empty())
    return {};
  const uint32_t appid = knowledge_appid(knowledge, game_id);
  if (appid == 0)
    return {};
  const auto userdata = platform->steam_userdata_dir();
  if (userdata.empty())
    return {};
  // The Steam account id: first non-zero numeric userdata subdir, sorted for
  // determinism across multi-account machines.
  std::error_code ec;
  std::vector<std::string> users;
  for (const auto& entry : std::filesystem::directory_iterator(userdata, ec)) {
    if (!entry.is_directory())
      continue;
    const std::string name = entry.path().filename().string();
    if (name.empty() || name == "0")
      continue;
    const bool numeric = std::all_of(name.begin(), name.end(), [](char c) {
      return std::isdigit(static_cast<unsigned char>(c)) != 0;
    });
    if (numeric)
      users.push_back(name);
  }
  if (ec || users.empty())
    return {};
  std::sort(users.begin(), users.end());
  return userdata / users.front() / std::to_string(appid) / subpath;
}

std::vector<std::string> save_extensions_for(const GameKnowledge& knowledge,
                                             const std::string& game_id) {
  const std::string declared = knowledge.get(game_id, "save_extensions", "");
  std::vector<std::string> out;
  std::string token;
  for (char c : declared + ",") {
    if (c == ',') {
      // Trim ASCII whitespace; drop empties ("dat,, ess" -> "dat","ess").
      std::size_t begin = 0;
      while (begin < token.size() &&
             std::isspace(static_cast<unsigned char>(token[begin])) != 0)
        ++begin;
      std::size_t end = token.size();
      while (end > begin &&
             std::isspace(static_cast<unsigned char>(token[end - 1])) != 0)
        --end;
      if (end > begin)
        out.push_back(token.substr(begin, end - begin));
      token.clear();
    } else {
      token.push_back(c);
    }
  }
  if (out.empty())
    return {"ess"};
  return out;
}

}  // namespace engine
