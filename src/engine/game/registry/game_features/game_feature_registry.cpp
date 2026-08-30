#include "engine/game/registry/game_features/game_feature_registry.h"

#include "engine/core/log/logger.h"
#include "engine/core/util/fs_utils.h"

#include <sstream>
#include <utility>

namespace engine {

namespace {
constexpr const char *kModDataCheckerType = "mod_data_checker";
constexpr const char *kGamePluginsType = "game_plugins";
} // namespace

namespace Game {
namespace Features {

Registry &Registry::instance() {
  static Registry registry;
  return registry;
}

bool Registry::register_feature(const std::string &game_id,
                                const std::string &feature_type, int priority,
                                std::shared_ptr<GameFeature> feature,
                                const std::string &source) {
  if (feature_type.empty() || !feature) {
    Logger::instance().warn(
        "Registry: refused registration with empty type/null feature");
    return false;
  }
  RegisteredGameFeature entry;
  entry.game_id = game_id;
  entry.feature_type = feature_type;
  entry.priority = priority;
  entry.source = source;
  entry.feature = std::move(feature);
  features_.push_back(std::move(entry));
  return true;
}

std::shared_ptr<GameFeature>
Registry::resolve(const std::string &game_id,
                  const std::string &feature_type) const {
  std::shared_ptr<GameFeature> best;
  int best_priority = 0;
  bool have_best = false;
  // Iterate in registration order; strictly higher priority replaces, equal
  // priority keeps the LAST registration (later plugin supersedes earlier).
  for (const auto &entry : features_) {
    if (entry.game_id != game_id || entry.feature_type != feature_type)
      continue;
    if (!have_best || entry.priority >= best_priority) {
      best = entry.feature;
      best_priority = entry.priority;
      have_best = true;
    }
  }
  return best;
}

std::shared_ptr<const ModDataCheckerFeature>
Registry::resolve_mod_data_checker(const std::string &game_id) const {
  std::vector<std::string> folders;
  std::vector<std::string> extensions;
  bool any = false;
  for (const auto &entry : features_) {
    if (entry.game_id != game_id || entry.feature_type != kModDataCheckerType)
      continue;
    auto *checker = dynamic_cast<ModDataCheckerFeature *>(entry.feature.get());
    if (!checker)
      continue;
    any = true;
    for (const auto &d : checker->folder_names())
      folders.push_back(d);
    for (const auto &e : checker->file_extensions())
      extensions.push_back(e);
  }
  if (!any)
    return nullptr;
  return std::make_shared<const ModDataCheckerFeature>(std::move(folders),
                                                       std::move(extensions));
}

std::shared_ptr<const GamePluginsFeature>
Registry::resolve_game_plugins(const std::string &game_id) const {
  return resolve_feature<GamePluginsFeature>(game_id);
}

std::vector<RegisteredGameFeature>
Registry::features_for(const std::string &game_id,
                       const std::string &feature_type) const {
  std::vector<RegisteredGameFeature> out;
  for (const auto &entry : features_) {
    if (entry.game_id != game_id || entry.feature_type != feature_type)
      continue;
    out.push_back(entry);
  }
  return out;
}

void Registry::clear() { features_.clear(); }

} // namespace Features
} // namespace Game

std::string native_plugins_csv(const GameKnowledge &knowledge,
                               const std::string &game_id) {
  if (auto feature =
          Game::Features::Registry::instance().resolve_game_plugins(game_id)) {
    std::string out;
    for (const auto &plugin : feature->plugins()) {
      if (plugin.empty())
        continue;
      if (!out.empty())
        out += ",";
      out += plugin;
    }
    return out;
  }
  return knowledge.get(game_id, "game_native_plugins", "");
}

std::vector<std::string> unmanaged_mods_for(const std::string &game_id) {
  if (auto feature = Game::Features::Registry::instance()
                         .resolve_feature<UnmanagedModsFeature>(game_id)) {
    return feature->mods();
  }
  return {};
}

// -- mod_data_content ------------------------------------------------

namespace {
const ModDataContentFeature::Content kPluginContent = {
    ModContentId::Plugin, "Plugins (ESP/ESM/ESL)", "plugin", false};
const ModDataContentFeature::Content kOptionalContent = {
    ModContentId::Optional, "Optional Plugins", "", true};
const ModDataContentFeature::Content kInterfaceContent = {
    ModContentId::Interface, "Interface", "interface", false};
const ModDataContentFeature::Content kMeshContent = {ModContentId::Mesh,
                                                     "Meshes", "mesh", false};
const ModDataContentFeature::Content kBsaContent = {
    ModContentId::Bsa, "Bethesda Archive", "bsa", false};
const ModDataContentFeature::Content kScriptContent = {
    ModContentId::Script, "Scripts (Papyrus)", "script", false};
const ModDataContentFeature::Content kSkseContent = {
    ModContentId::Skse, "Script Extender Plugin", "skse", false};
const ModDataContentFeature::Content kSkseFilesContent = {
    ModContentId::SkseFiles, "Script Extender Files", "", true};
const ModDataContentFeature::Content kSkyprocContent = {
    ModContentId::Skyproc, "SkyProc Patcher", "skyproc", false};
const ModDataContentFeature::Content kSoundContent = {
    ModContentId::Sound, "Sound or Music", "sound", false};
const ModDataContentFeature::Content kTextureContent = {
    ModContentId::Texture, "Textures", "texture", false};
const ModDataContentFeature::Content kMcmContent = {
    ModContentId::Mcm, "MCM Configuration", "mcm", false};
const ModDataContentFeature::Content kIniContent = {ModContentId::Ini,
                                                    "INI Files", "ini", false};
const ModDataContentFeature::Content kFacegenContent = {
    ModContentId::Facegen, "FaceGen Data", "facegen", false};
const ModDataContentFeature::Content kModgroupContent = {
    ModContentId::Modgroup, "ModGroup Files", "modgroup", false};
} // namespace

std::vector<ModDataContentFeature::Content> mod_content_catalog() {
  return {kPluginContent, kOptionalContent,  kInterfaceContent,
          kMeshContent,   kBsaContent,       kScriptContent,
          kSkseContent,   kSkseFilesContent, kSkyprocContent,
          kSoundContent,  kTextureContent,   kMcmContent,
          kIniContent,    kFacegenContent,   kModgroupContent};
}

int mod_content_id_from_string(const std::string &id) {
  const std::string lc = toLower(id);
  if (lc == "plugin")
    return ModContentId::Plugin;
  if (lc == "optional")
    return ModContentId::Optional;
  if (lc == "interface")
    return ModContentId::Interface;
  if (lc == "mesh")
    return ModContentId::Mesh;
  if (lc == "bsa")
    return ModContentId::Bsa;
  if (lc == "script")
    return ModContentId::Script;
  if (lc == "skse")
    return ModContentId::Skse;
  if (lc == "skse_files")
    return ModContentId::SkseFiles;
  if (lc == "skyproc")
    return ModContentId::Skyproc;
  if (lc == "sound")
    return ModContentId::Sound;
  if (lc == "texture")
    return ModContentId::Texture;
  if (lc == "mcm")
    return ModContentId::Mcm;
  if (lc == "ini")
    return ModContentId::Ini;
  if (lc == "facegen")
    return ModContentId::Facegen;
  if (lc == "modgroup")
    return ModContentId::Modgroup;
  return -1;
}

std::string mod_content_string_from_id(int id) {
  for (const auto &c : mod_content_catalog())
    if (c.id == id)
      return c.name;
  return "";
}

std::vector<ModDataContentFeature::Content>
ModDataContentFeature::all_contents() const {
  std::vector<Content> catalog = mod_content_catalog();
  std::vector<Content> out;
  for (auto &c : catalog) {
    for (const auto &cc : custom_contents_)
      if (cc.id == c.id) {
        c = cc;
        break;
      }
    if (std::find(enabled_ids_.begin(), enabled_ids_.end(), c.id) !=
        enabled_ids_.end()) {
      out.push_back(std::move(c));
    }
  }
  return out;
}

namespace {
bool content_enabled(const std::vector<int> &enabled, int id) {
  return std::find(enabled.begin(), enabled.end(), id) != enabled.end();
}
} // namespace

std::vector<int> ModDataContentFeature::contents_for(
    const std::shared_ptr<const FileTree> &tree,
    const std::string &script_extender_plugin_path) const {
  std::vector<int> out;
  if (!tree)
    return out;

  auto push = [&](int id) {
    if (content_enabled(enabled_ids_, id))
      out.push_back(id);
  };

  for (const auto &entry : *tree) {
    if (entry->is_file()) {
      const std::string suf = toLower(entry->suffix());
      if (suf == "esp" || suf == "esm" || suf == "esl") {
        push(ModContentId::Plugin);
      } else if (suf == "bsa" || suf == "ba2") {
        push(ModContentId::Bsa);
      } else if (suf == "ini" && entry->compare("meta.ini") != 0) {
        push(ModContentId::Ini);
      } else if (suf == "modgroups") {
        push(ModContentId::Modgroup);
      }
    } else {
      const std::string n = toLower(entry->name());
      if (n == "textures" || n == "icons" || n == "bookart") {
        push(ModContentId::Texture);
      } else if (n == "meshes") {
        push(ModContentId::Mesh);
      } else if (n == "interface" || n == "menus") {
        push(ModContentId::Interface);
      } else if (n == "music" || n == "sound") {
        push(ModContentId::Sound);
      } else if (n == "scripts") {
        push(ModContentId::Script);
      } else if (n == "skyproc patchers") {
        push(ModContentId::Skyproc);
      } else if (n == "mcm") {
        push(ModContentId::Mcm);
      } else if (n == "optional") {
        // MO2: an "Optional" folder only counts when it has contents.
        auto sub = entry->as_tree();
        if (sub && sub->size() > 0)
          push(ModContentId::Optional);
      }
    }
  }

  if (content_enabled(enabled_ids_, ModContentId::Facegen)) {
    if (tree->find_directory("meshes/actors/character/facegendata") ||
        tree->find_directory("textures/actors/character/facegendata")) {
      out.push_back(ModContentId::Facegen);
    }
  }

  if (!script_extender_plugin_path.empty()) {
    auto e = tree->find_directory(script_extender_plugin_path);
    if (e) {
      if (content_enabled(enabled_ids_, ModContentId::SkseFiles)) {
        out.push_back(ModContentId::SkseFiles);
      }
      if (content_enabled(enabled_ids_, ModContentId::Skse)) {
        for (const auto &f : *e) {
          if (f->has_suffix("dll")) {
            out.push_back(ModContentId::Skse);
            break;
          }
        }
      }
    }
  }

  return out;
}

// -- register_game_feature_data (the 7 structured-data feature types) --

namespace {

// First value for `key`, or "" when absent.
std::string kv_get(const std::vector<std::pair<std::string, std::string>> &kv,
                   const std::string &key) {
  for (const auto &[k, v] : kv)
    if (k == key)
      return v;
  return "";
}

void split_csv(const std::string &s, std::vector<std::string> &out) {
  std::istringstream ss(s);
  std::string part;
  while (std::getline(ss, part, ',')) {
    auto st = part.find_first_not_of(" \t");
    auto en = part.find_last_not_of(" \t");
    if (st == std::string::npos)
      continue;
    out.push_back(part.substr(st, en - st + 1));
  }
}

} // namespace

bool register_game_feature_data(
    const std::string &game_id, const std::string &feature_type, int priority,
    const std::vector<std::pair<std::string, std::string>> &kv,
    const std::string &source) {
  if (game_id.empty() || feature_type.empty()) {
    Logger::instance().warn(
        "register_game_feature_data: empty game_id/feature_type - ignored");
    return false;
  }

  std::shared_ptr<GameFeature> feature;
  if (feature_type == "data_archives") {
    std::vector<std::string> archives;
    split_csv(kv_get(kv, "vanilla_archives"), archives);
    feature = std::make_shared<DataArchivesFeature>(std::move(archives));
  } else if (feature_type == "script_extender") {
    feature = std::make_shared<ScriptExtenderFeature>(
        kv_get(kv, "binary"), kv_get(kv, "plugin_path"),
        kv_get(kv, "loader_name"), kv_get(kv, "savegame_extension"));
  } else if (feature_type == "save_game_info") {
    std::vector<std::string> extensions;
    split_csv(kv_get(kv, "extensions"), extensions);
    feature = std::make_shared<SaveGameInfoFeature>(std::move(extensions));
  } else if (feature_type == "local_savegames") {
    feature = std::make_shared<LocalSavegamesFeature>(
        kv_get(kv, "saves_subpath"), kv_get(kv, "ini_file"));
  } else if (feature_type == "unmanaged_mods") {
    std::vector<std::string> mods;
    split_csv(kv_get(kv, "mods"), mods);
    feature = std::make_shared<UnmanagedModsFeature>(std::move(mods));
  } else if (feature_type == "bsa_invalidation") {
    feature = std::make_shared<BSAInvalidationFeature>(
        kv_get(kv, "bsa_name"), kv_get(kv, "bsa_version"));
  } else if (feature_type == "mod_data_content") {
    std::vector<int> enabled;
    std::vector<std::string> ids;
    split_csv(kv_get(kv, "enabled"), ids);
    for (const auto &id : ids) {
      int v = mod_content_id_from_string(id);
      if (v >= 0)
        enabled.push_back(v);
    }
    std::vector<ModDataContentFeature::Content> custom;
    const std::string prefix = "content:";
    for (const auto &[k, v] : kv) {
      if (k.rfind(prefix, 0) != 0)
        continue;
      int id = mod_content_id_from_string(k.substr(prefix.size()));
      if (id < 0)
        continue;
      ModDataContentFeature::Content c;
      c.id = id;
      const size_t p1 = v.find('|');
      if (p1 == std::string::npos) {
        c.name = v;
      } else {
        c.name = v.substr(0, p1);
        const size_t p2 = v.find('|', p1 + 1);
        if (p2 == std::string::npos) {
          c.icon = v.substr(p1 + 1);
        } else {
          c.icon = v.substr(p1 + 1, p2 - p1 - 1);
          const std::string flag = toLower(v.substr(p2 + 1));
          c.filter_only = (flag == "1" || flag == "true" || flag == "filter");
        }
      }
      custom.push_back(std::move(c));
    }
    feature = std::make_shared<ModDataContentFeature>(std::move(enabled),
                                                      std::move(custom));
  } else {
    Logger::instance().warn(
        "register_game_feature_data: unknown feature type '" + feature_type +
        "' - ignored");
    return false;
  }

  return Game::Features::Registry::instance().register_feature(
      game_id, feature_type, priority, std::move(feature), source);
}

} // namespace engine
