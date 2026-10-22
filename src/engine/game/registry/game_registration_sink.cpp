#include "engine/game/registry/game_registration_sink.h"

// The ABI-typed registries. Including these is what gives this translation
// unit the real ABI callback types (each header pulls gmm_abi_v2.h
// transitively); this file never includes gmm_abi_*.h directly.
#include "engine/game/registry/game_features/game_feature.h"
#include "engine/game/registry/game_features/game_feature_registry.h"
#include "engine/log/logger.h"
#include "engine/plugin_host/category_factory.h"
#include "engine/plugin_host/deploy_strategy_registry.h"
#include "engine/plugin_host/diagnose_registry.h"
#include "engine/plugin_host/file_mapper_registry.h"
#include "engine/plugin_host/hook_registry.h"
#include "engine/plugin_host/order_encoding_registry.h"
#include "engine/plugin_host/plugin_settings_registry.h"
#include "engine/plugin_host/requirements_registry.h"
#include "engine/plugin_host/tool_registry.h"
#include "engine/sort/sorter/abi.h"
#include "engine/sort/sorter/registry.h"
#include "ui/preview/preview_registry.h"

#include <cctype>
#include <filesystem>
#include <utility>

namespace engine {

// Convert snake_case tool id to Title Case for display (e.g. "event_bus_viewer"
// -> "Event Bus Viewer").
static std::string tool_id_to_display_name(const std::string &id) {
  std::string result;
  result.reserve(id.size());
  bool capitalise = true;
  for (char c : id) {
    if (c == '_') {
      capitalise = true;
      result += ' ';
    } else if (capitalise) {
      result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      capitalise = false;
    } else {
      result += c;
    }
  }
  return result;
}

void RegistrationSink::add_entity(const GameDescriptor &desc,
                                  const std::string &dup_check_game_id) {
  // Per-record identity (v3 P1): every register_game / register_identity
  // call appends one record. An unset id falls back to the plugin's
  // current game_id (the .so stem seed), matching the old flat behavior
  // for a null game_id; an unset display_name falls back to the record's
  // own id.
  PluginInfo::GameRecord rec;
  rec.game_id           = !desc.id.empty() ? desc.id : plugin_->game_id;
  rec.game_display_name = !desc.display_name.empty() ? desc.display_name : rec.game_id;
  rec.steam_appid       = desc.steam_appid.value_or(0u);
  rec.nexus_domain      = desc.nexus_domain.value_or(std::string());
  rec.gog_id            = desc.gog_id.value_or(std::string());
  rec.epic_namespace    = desc.epic_namespace.value_or(std::string());
  rec.exe_windows       = desc.exe_windows.value_or(std::string());
  rec.exe_linux         = desc.exe_linux.value_or(std::string());
  rec.exe_macos         = desc.exe_macos.value_or(std::string());

  // Duplicate game_id within one plugin's registration is a programming
  // error: refuse the record loudly, keep the earlier registration.
  for (const auto &existing : plugin_->games) {
    if (existing.game_id == rec.game_id) {
      Logger::instance().error("Plugin " + plugin_->path + " registered game_id '" +
                               rec.game_id + "' twice; duplicate registration refused");
      return;
    }
  }

  // register_game / register_identity is THE game-support marker: only the
  // first plugin to register a given game_id gets game_support = true;
  // secondary registrants (e.g. a Tool plugin that also registers the game to
  // scope its sort provider) still get their record but don't appear in
  // the "Create New Instance" game list. Directory load is sorted (v3 P1),
  // so "first" is deterministic: filename order.
  bool duplicate = false;
  if (loader_) {
    for (const auto &other : loader_->plugins()) {
      if (&other == plugin_)
        continue;
      for (const auto &other_rec : other.games) {
        if (other_rec.game_support && other_rec.game_id == dup_check_game_id) {
          duplicate = true;
          break;
        }
      }
      if (duplicate)
        break;
    }
  }
  rec.game_support = !duplicate;
  if (duplicate) {
    Logger::instance().warn("Plugin " + plugin_->path + " registered game_id '" +
                            dup_check_game_id +
                            "' already owned by another plugin; game support refused");
  }

  plugin_->games.push_back(std::move(rec));
  // The flat identity fields mirror games[0] for compatibility with every
  // existing single-game reader.
  plugin_->apply_game_record(plugin_->games.front());

  const auto &stored = plugin_->games.back();
  Logger::instance().debug("Plugin registered game: id=" + stored.game_id +
                           " name=" + stored.game_display_name + " nexus=" +
                           (desc.nexus_domain ? *desc.nexus_domain : "none"));
}

void RegistrationSink::plugin_meta(const char *name, const char *author,
                                   const char *version, const char *description) {
  if (name)
    plugin_->plugin_name = name;
  if (author)
    plugin_->author = author;
  if (version)
    plugin_->version = version;
  if (description)
    plugin_->description = description;

  // Map the plugin's own name to its basename so host_get_setting(plugin_name,
  // key) resolves to the same QSettings store the UI persists under.
  if (name) {
    const std::string basename =
        std::filesystem::path(plugin_->path).filename().string();
    PluginSettingsRegistry::instance().register_alias(name, basename);
  }
}

void RegistrationSink::settings_register(const char *const *keys,
                                         const char *const *values, size_t count,
                                         const char *name_alias) {
  auto &settings = plugin_->settings;
  for (size_t i = 0; i < count; ++i) {
    if (!keys[i] || !values[i])
      continue;
    settings.emplace_back(keys[i], values[i]);
  }

  // Persist the declaration in the process-wide settings registry so the host
  // callbacks can read/write these keys at runtime (v2 parity for v1 plugins).
  const std::string basename = std::filesystem::path(plugin_->path).filename().string();
  PluginSettingsRegistry::instance().register_settings(basename, keys, values, count);
  PluginSettingsRegistry::instance().register_alias(plugin_->game_id, basename);
  if (name_alias)
    PluginSettingsRegistry::instance().register_alias(name_alias, basename);
}

void RegistrationSink::settings_tab_register(const char *title, const char *const *keys,
                                             const char *const *types,
                                             const char *const *defaults,
                                             const char *const *options, size_t count,
                                             const char *name_alias) {
  PluginInfo::SettingTab tab;
  tab.title = title;
  for (size_t i = 0; i < count; ++i) {
    if (!keys[i] || !types[i])
      continue;
    PluginInfo::SettingTabEntry entry;
    entry.key  = keys[i];
    entry.type = types[i];
    if (defaults && defaults[i])
      entry.default_value = defaults[i];
    if (options && options[i]) {
      if (entry.type == "choice") {
        // newline-separated candidate choices
        std::string opts = options[i];
        size_t pos       = 0;
        while ((pos = opts.find('\n')) != std::string::npos) {
          entry.choices.emplace_back(opts.substr(0, pos));
          opts.erase(0, pos + 1);
        }
        if (!opts.empty())
          entry.choices.emplace_back(std::move(opts));
      } else if (entry.type == "int") {
        entry.int_range = options[i];
      }
    }
    tab.settings.push_back(std::move(entry));
  }
  plugin_->settings_tab = std::move(tab);

  // Persist the declaration in the process-wide settings registry so the host
  // callbacks can read/write these keys at runtime (v2 parity for v1 plugins).
  const std::string basename = std::filesystem::path(plugin_->path).filename().string();
  PluginSettingsRegistry::instance().register_settings_tab(basename, title, keys, types,
                                                           defaults, options, count);
  PluginSettingsRegistry::instance().register_alias(plugin_->game_id, basename);
  if (name_alias)
    PluginSettingsRegistry::instance().register_alias(name_alias, basename);
}

void RegistrationSink::requirements_register(void *fn, void *user_data) {
  // Defer evaluation: store the provider in the process-wide registry and
  // let the loader run check_requirements() once every plugin is loaded, so
  // cross-plugin / cross-game dependencies can be resolved correctly (a
  // requirement declared by plugin A may only be satisfiable by plugin B,
  // which loads later). The provider is keyed by plugin path so it can be
  // dropped on unload before dlclose.
  RequirementsRegistry::instance().register_requirements(
      plugin_->path, reinterpret_cast<GmmRequirementsFn>(fn), user_data);
}

void RegistrationSink::diagnostics_register(const char *game_id, void *fn,
                                            void *user_data) {
  std::string gid = game_id ? game_id : plugin_->game_id;
  PluginDiagnostics d;
  d.game_id   = gid;
  d.fn        = fn;
  d.user_data = user_data;
  plugin_->diagnostics_v2.push_back(std::move(d));

  // Register into the process-wide v2 DiagnoseRegistry so the engine can
  // collect problems for this game via DiagnoseRegistry::collect_diagnostics.
  DiagnoseRegistry::instance().register_diagnostics(
      gid, reinterpret_cast<GmmDiagnoseFn>(fn), user_data, plugin_->path);

  Logger::instance().debug("Plugin registered v2 diagnostics for game=" + gid);
}

void RegistrationSink::game_feature_register(const char *game_id,
                                             const char *feature_type, int priority,
                                             const char *const *folder_names,
                                             size_t folder_count,
                                             const char *const *file_extensions,
                                             size_t extension_count) {
  // NULL game_id = this plugin's own game (matches how every other
  // registration keys itself); an explicit game_id lets a standalone plugin
  // override a game it doesn't provide (the P1.2 override test does this).
  std::string gid  = game_id ? game_id : plugin_->game_id;
  std::string type = feature_type ? feature_type : "";
  if (gid.empty() || type.empty()) {
    Logger::instance().warn("Game feature registered with empty game_id/type");
    return;
  }

  if (type == "mod_data_checker") {
    std::vector<std::string> folders;
    if (folder_names) {
      for (size_t i = 0; i < folder_count; ++i)
        if (folder_names[i])
          folders.emplace_back(folder_names[i]);
    }
    std::vector<std::string> extensions;
    if (file_extensions) {
      for (size_t i = 0; i < extension_count; ++i)
        if (file_extensions[i])
          extensions.emplace_back(file_extensions[i]);
    }
    auto checker = std::make_shared<ModDataCheckerFeature>(std::move(folders),
                                                           std::move(extensions));
    Game::Features::Registry::instance().register_feature(
        gid, type, priority, std::move(checker), plugin_->path);
  } else if (type == "game_plugins") {
    // The game's vanilla plugin files (MO2 GamePlugins::gamePlugins()):
    // Skyrim's ESMs + _ResourcePack.esl head the unmanaged top band. The
    // plugin names ride the folder_names array slot - the ABI's two string
    // arrays are generic payload slots interpreted per feature type.
    std::vector<std::string> plugins;
    if (folder_names) {
      for (size_t i = 0; i < folder_count; ++i)
        if (folder_names[i])
          plugins.emplace_back(folder_names[i]);
    }
    auto feature = std::make_shared<GamePluginsFeature>(std::move(plugins));
    Game::Features::Registry::instance().register_feature(
        gid, type, priority, std::move(feature), plugin_->path);
  } else {
    Logger::instance().warn("Plugin registered unknown game feature type: " + type +
                            " (ignored)");
    return;
  }

  Logger::instance().debug("Plugin registered game feature: " + type + " (game=" + gid +
                           ", priority=" + std::to_string(priority) + ")");
}

void RegistrationSink::game_feature_data_register(const char *game_id,
                                                  const char *feature_type,
                                                  int priority, const char *const *keys,
                                                  const char *const *values,
                                                  size_t count) {
  // NULL game_id = this plugin's own game (same rule as every other
  // registration); an explicit game_id lets a standalone plugin override a
  // game it doesn't provide.
  std::string gid  = game_id ? game_id : plugin_->game_id;
  std::string type = feature_type ? feature_type : "";
  if (gid.empty() || type.empty()) {
    Logger::instance().warn("Game feature data registered with empty game_id/type");
    return;
  }

  // The 7 structured-data feature types (mod_data_content, data_archives,
  // script_extender, save_game_info, local_savegames, unmanaged_mods,
  // bsa_invalidation) parse through the shared registry function, so the
  // key-value contract lives once and the pybind mirror stays identical.
  std::vector<std::pair<std::string, std::string>> kv;
  if (keys && values) {
    for (size_t i = 0; i < count; ++i)
      if (keys[i])
        kv.emplace_back(keys[i], values[i] ? values[i] : "");
  }
  if (!register_game_feature_data(gid, type, priority, std::move(kv), plugin_->path)) {
    return;  // register_game_feature_data already logged the reason
  }

  Logger::instance().debug("Plugin registered game feature: " + type + " (game=" + gid +
                           ", priority=" + std::to_string(priority) + ")");
}

void RegistrationSink::knowledge_set(const std::string &game_id, const std::string &key,
                                     const std::string &value) {
  loader_->knowledge().set(game_id, key, value);
}

void RegistrationSink::hook_register(const std::string &hook_tag,
                                     const std::string &hook_data, void *fn,
                                     int priority, void *user_data) {
  // Register the hook function into HookRegistry so the engine can fire
  // it at pipeline points (before_deploy, after_scan, conflict_resolution).
  // Note: the ABI hook fn takes void* data, so we const_cast the string data.
  // (GmmHookFn and GmmHookFnV2 are the same signature.)
  loader_->hook_registry().register_hook(
      hook_tag,
      [fn, user_data, hook_tag, hook_data](Mod &mod, PipelineContext &ctx) {
        (void)mod;
        (void)ctx;
        if (fn)
          reinterpret_cast<GmmHookFnV2>(fn)(
              hook_tag.c_str(), const_cast<char *>(hook_data.c_str()), user_data);
      },
      priority, plugin_->path);
}

void RegistrationSink::hook_register_v2(const char *tag, void *fn, int priority,
                                        void *user_data, const char *plugin_path) {
  // Also register the raw v2 ABI hook into the v2 HookRegistry singleton so
  // the pipeline can dispatch it directly via
  // HookRegistry::instance().dispatch() with an arbitrary data pointer (the v2
  // GmmHookFnV2 contract).
  ::HookRegistry::instance().register_hook(tag, reinterpret_cast<GmmHookFnV2>(fn),
                                           priority, user_data, plugin_path);
}

void RegistrationSink::order_encoding_register(void *fn, void *user_data) {
  const std::string &game_id        = plugin_->game_id;
  plugin_->order_encoding_fn        = reinterpret_cast<GmmOrderEncodingFnV2>(fn);
  plugin_->order_encoding_user_data = user_data;

  // Store the callback in the order-encoding registry so the pipeline can
  // retrieve and call it when writing load-order files (plugins.txt, ...).
  OrderEncodingRegistry::instance().register_provider(
      game_id, reinterpret_cast<GmmOrderEncodingFnV2>(fn), user_data, plugin_->path);

  Logger::instance().debug("Plugin registered v2 order encoding hook (game=" + game_id +
                           ")");
}

void RegistrationSink::deploy_strategy_register(void *deploy_fn, void *remove_fn,
                                                void *user_data) {
  const std::string &game_id = plugin_->game_id;
  plugin_->deploy_fn         = reinterpret_cast<GmmDeployFnV2>(deploy_fn);
  plugin_->remove_fn         = reinterpret_cast<GmmRemoveFnV2>(remove_fn);
  plugin_->deploy_user_data  = user_data;

  // Store the callbacks in the deploy-strategy registry so the pipeline can
  // retrieve and call them instead of the built-in DeploymentStrategy.
  DeployStrategyRegistry::instance().register_provider(
      game_id, reinterpret_cast<GmmDeployFnV2>(deploy_fn),
      reinterpret_cast<GmmRemoveFnV2>(remove_fn), user_data, plugin_->path);

  Logger::instance().debug("Plugin registered v2 deploy strategy (game=" + game_id +
                           ")");
}

void RegistrationSink::file_mapper_register(const char *game_id, void *fn,
                                            void *user_data) {
  std::string gid = game_id ? game_id : plugin_->game_id;
  PluginFileMapper m;
  m.game_id   = gid;
  m.fn        = fn;
  m.user_data = user_data;
  plugin_->file_mappers.push_back(std::move(m));

  // Also register into the process-wide FileMapperRegistry so the deploy
  // pipeline can aggregate virtual file overlays across all plugins via
  // FileMapperRegistry::instance().get_mappings(game_id). The registry stores
  // the raw GmmFileMapperFn + user_data and drops it on unload (clear_plugin),
  // so the pointer never outlives the .so that owns fn.
  FileMapperRegistry::instance().register_mapper(
      gid, reinterpret_cast<GmmFileMapperFn>(fn), user_data, plugin_->path);

  Logger::instance().debug("Plugin registered v2 file mapper for game=" + gid);
}

void RegistrationSink::sort_provider_register(const std::string &game_id, void *sort_fn,
                                              void *user_data) {
  auto provider = std::make_unique<Sorter::Abi>(
      game_id.c_str(), reinterpret_cast<SortFn>(sort_fn), user_data);
  Sorter::Registry::instance().register_provider(game_id, std::move(provider));
}

void RegistrationSink::stage_claim_register(const std::string &game_id,
                                            const std::string &stage_name,
                                            StageFn handler, int priority) {
  loader_->stage_registry().register_claim(game_id, stage_name, std::move(handler),
                                           priority, plugin_->path);
}

void RegistrationSink::preview_register(const char *file_extension, void *preview_data,
                                        void *fn, void *user_data) {
  const std::string ext_str = file_extension ? file_extension : "";

  PluginPreview p;
  p.file_extension = ext_str;
  p.preview_data   = preview_data;
  p.fn             = fn;
  p.user_data      = user_data;
  plugin_->previews.push_back(std::move(p));

  // Mirror the registration into the UI-side Registry so the preview
  // window can embed the plugin-provided QWidget* for this extension. The
  // registry stores the opaque fn + user_data; the engine never sees QWidget.
  Logger::instance().debug("[PluginLoader] cb_v2_register_preview: calling "
                           "Registry::register_preview ext=" +
                           ext_str);
  ui::preview::Registry::instance().register_preview(
      ext_str, reinterpret_cast<GmmPreviewFn>(fn), preview_data, user_data,
      plugin_->path);

  Logger::instance().debug("[PluginLoader] cb_v2_register_preview: DONE ext=" +
                           ext_str);
}

ExternalTool RegistrationSink::tool_register(const char *tool_id, const char *kind,
                                             void *invoke_fn, void *user_data,
                                             bool callback_registry) {
  ExternalTool tool;
  tool.tool_id      = tool_id ? tool_id : "";
  tool.game_id      = plugin_->game_id;
  tool.display_name = tool_id_to_display_name(tool.tool_id);

  std::string kind_str = kind ? kind : "advisory";
  tool.kind = (kind_str == "workshop") ? ToolKind::Workshop : ToolKind::Advisory;

  if (invoke_fn) {
    tool.invoke_fn = [invoke_fn](void *ud) {
      reinterpret_cast<ToolInvokeFn>(invoke_fn)(ud);
    };
    tool.invoke_user_data = user_data;
  }

  // Plugins that never called register_game have game_support=false - register
  // their tools as global
  if (!plugin_->game_support) {
    tool.game_id = "";
  }

  loader_->tool_registry().register_tool(tool);

  if (callback_registry) {
    // Also register into the v2 IPluginTool registry - the canonical store of
    // plugin-provided tool callbacks (raw fn + user_data + owning plugin path,
    // used for unload cleanup). The platform/tools ToolRegistry above drives
    // the Tools menu; this one is the v2 source of truth.
    PluginToolRegistry::instance().register_tool(
        tool.tool_id, kind_str, reinterpret_cast<ToolInvokeFn>(invoke_fn), user_data,
        plugin_->path);
  }

  return tool;
}

void RegistrationSink::modpage_register(const char *url, void *fn, void *user_data) {
  PluginModPage m;
  m.url       = url ? url : "";
  m.fn        = fn;
  m.user_data = user_data;
  // Capture url before moving m into the plugin's modpages vector.
  const std::string modpage_url = m.url;
  plugin_->modpages.push_back(std::move(m));

  Logger::instance().debug("Plugin registered v2 modpage for url=" + modpage_url);
}

void RegistrationSink::save_parser_register(const std::string &game_id, SaveParserFn fn,
                                            int priority, void *user_data) {
  SaveParserRegistry::instance().register_parser(game_id, priority, std::move(fn),
                                                 user_data, plugin_->path);
}

void RegistrationSink::animation_parser_register(
    const std::string &game_id, std::shared_ptr<AnimationParserFeature> feature,
    int priority, const std::string &source) {
  Game::Features::Registry::instance().register_feature(
      game_id, "animation_parser", priority, std::move(feature), source);
}

void RegistrationSink::game_validator_register(const char *game_id, void *fn,
                                               void *user_data) {
  std::string gid = game_id ? game_id : plugin_->game_id;
  if (gid.empty()) {
    Logger::instance().warn("Game validator registered with empty game_id or null fn");
    return;
  }

  // Engine-side: store the validator for the engine's game-detection path
  // to consult when it next needs to test whether a directory looks like a
  // valid install of this game. Today the detector (game_detector.cpp) only
  // looks at Steam libraryfolders.vdf + appmanifest; wiring the ABI fn in
  // there is the cheap-next-step. For now we capture it on the plugin so
  // the next detection pass can use it (or a debug tool can introspect it).
  // ponytail: hook into game detection when we need non-Steam installs.
  plugin_->game_validator_fn        = fn;
  plugin_->game_validator_user_data = user_data;
  Logger::instance().debug("Plugin registered v2 game validator for game=" + gid);
}

void RegistrationSink::game_variant_register(const char *game_id,
                                             const char *variant_id,
                                             const char *display_name) {
  std::string gid = game_id ? game_id : plugin_->game_id;
  std::string vid = variant_id ? variant_id : "";
  if (gid.empty() || vid.empty()) {
    Logger::instance().warn("Game variant registered with empty game_id or variant_id");
    return;
  }

  // Capture on the plugin's variant list. The instance/game model and
  // create-instance flow consult this list to offer "Steam / GOG / Epic"
  // disambiguation when a game has more than one variant. Mirrors
  // IPluginGame::setGameVariant in MO2.
  plugin_->variants.push_back({std::move(vid), display_name ? display_name : ""});
}

void RegistrationSink::save_overlay_register(const char *game_id, void *fn,
                                             int priority, void *user_data) {
  std::string gid = game_id ? game_id : plugin_->game_id;
  if (gid.empty()) {
    Logger::instance().warn("Save overlay registered with empty game_id or null fn");
    return;
  }

  // The Saves tab invokes this on every parsed save. Higher-priority
  // overlays win; the engine currently keeps the LAST registration
  // (plugin load order is the priority) - good enough for the single-
  // overlay-per-game contract MO2 uses. Stored as void* in PluginInfo
  // so this header doesn't drag the full v2 header in.
  plugin_->save_overlay_fn        = fn;
  plugin_->save_overlay_user_data = user_data;
}

void RegistrationSink::category_register(const char *category) {
  if (category)
    plugin_->category = category;
}

void RegistrationSink::categories_register(const PluginInfo *plugin, const int *ids,
                                           const char *const *names,
                                           const int *parent_ids, size_t count) {
  if (!ids || !names || count == 0)
    return;

  // Recorded against the declaring game, never pushed straight into the
  // active set: every game plugin is loaded into this process regardless of
  // which game is on screen, so an unscoped merge here is how a Skyrim
  // instance ended up showing Isaac's categories.
  const std::string game_id = plugin ? plugin->game_id : "";
  Category::Factory::instance().merge(game_id, ids, names, parent_ids, count);

  if (plugin) {
    Logger::instance().debug("Plugin registered " + std::to_string(count) +
                             " categories (plugin=" + plugin->game_id + ")");
  }
}

void RegistrationSink::capability_register(
    const char *capability, const char *display_name, const char *data_path,
    const char *description, const char *protocol_handler, const char *website_domain,
    const char *supported_platforms, const char *insert_before,
    const char *insert_after) {
  CapabilityInfo info;
  info.game_id          = plugin_->game_id;
  info.capability       = capability ? capability : "";
  info.display_name     = display_name ? display_name : capability ? capability : "";
  info.data_path        = data_path ? data_path : "";
  info.description      = description ? description : "";
  info.protocol_handler = protocol_handler ? protocol_handler : "";
  info.website_domain   = website_domain ? website_domain : "";
  info.insert_before    = insert_before ? insert_before : "";
  info.insert_after     = insert_after ? insert_after : "";

  // Parse comma-separated platforms
  if (supported_platforms) {
    std::string platforms_str = supported_platforms;
    size_t pos                = 0;
    while ((pos = platforms_str.find(',')) != std::string::npos) {
      info.supported_platforms.push_back(platforms_str.substr(0, pos));
      platforms_str.erase(0, pos + 1);
    }
    if (!platforms_str.empty()) {
      info.supported_platforms.push_back(platforms_str);
    }
  }

  loader_->capabilities().register_capability(info);
}

}  // namespace engine
