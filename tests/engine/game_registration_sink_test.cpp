// v3 P0 RegistrationSink parity test (Workspace-tnmx). The v1/v2 cb_* handlers
// in plugin_loader.cpp must produce EXACTLY the engine state that a direct
// GameDescriptor + RegistrationSink replay produces - that is the whole P0
// contract: the sink is the single effect path, the handlers only compute
// strings and marshal ABI types.
//
// Route A: load the real SkyrimSpecialEdition .so through the ABI bridge.
// Route B: replay its registration calls straight onto a RegistrationSink.
// Both snapshots must be equal (fn pointers + dlopen handle excluded - they
// cannot be equal by construction).
//
// Uses the check() PASS/FAIL pattern (Release builds compile out assert()).

#include "engine/game/registry/game_descriptor.h"
#include "engine/game/registry/game_registration_sink.h"
#include "engine/plugin_host/order_encoding_registry.h"
#include "engine/plugin_host/plugin_loader.h"
#include "engine/plugin_host/plugin_settings_registry.h"
#include "engine/plugin_host/save_parser_registry.h"
#include "engine/plugin_host/tool_registry.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <tuple>
#include <vector>

#ifndef GMM_SKYRIM_PLUGIN_PATH
#define GMM_SKYRIM_PLUGIN_PATH "SkyrimSpecialEdition.so"
#endif
#ifndef GMM_SKYRIM_PLUGIN_VERSION
#define GMM_SKYRIM_PLUGIN_VERSION "0.0.0"
#endif

namespace fs = std::filesystem;

using engine::DescriptorProvenance;
using engine::GameDescriptor;
using engine::OrderEncodingRegistry;
using engine::PluginInfo;
using engine::PluginLoader;
using engine::PluginToolRegistry;
using engine::RegistrationSink;
using engine::SaveGame;
using engine::SaveParserRegistry;

namespace {

void check(bool cond, const std::string &msg) {
  INFO(msg);
  REQUIRE(cond);
}

// Serialized engine state produced by one registration route. Everything in
// here is comparable across routes: no function pointers, no dlopen handles.
struct Snapshot {
  // PluginInfo identity + metadata
  std::string path;
  std::string game_id;
  std::string game_display_name;
  std::string plugin_name;
  std::string author;
  std::string version;
  std::string description;
  std::string category;
  std::string nexus_domain;
  std::string gog_id;
  std::string epic_namespace;
  std::string exe_windows;
  std::string exe_linux;
  std::string exe_macos;
  uint32_t steam_appid = 0;
  uint32_t abi_version = 0;
  uint64_t features    = 0;
  bool game_support    = false;
  bool registered      = false;
  bool loaded          = false;

  // Settings (Skyrim declares none - both routes must agree on empty)
  std::vector<std::pair<std::string, std::string>> settings;

  // Game knowledge: sorted (key, value) rows for the plugin's game
  std::vector<std::pair<std::string, std::string>> knowledge;

  // Engine hook registry rows: sorted (tag, priority, plugin_id) per tag
  std::vector<std::tuple<std::string, int, std::string>> hooks;

  // Capability tabs: sorted serialized CapabilityInfo rows
  std::vector<std::string> capabilities;

  // Save parsers registered (sorted game_ids with a parser)
  std::vector<std::string> save_parsers;

  // Save overlays: sorted game_ids with a non-null overlay fn
  std::vector<std::string> save_overlays;

  // Game variants: sorted "variant_id|display_name" rows
  std::vector<std::string> variants;

  // LOOT tool: serialized ExternalTool + PluginToolRegistry row
  std::string tool;

  // Order encoding provider: present + user_data null + plugin path
  std::string order_encoding;
};

Snapshot snapshot(const PluginLoader &loader, const PluginInfo &info) {
  Snapshot s;
  s.path              = info.path;
  s.game_id           = info.game_id;
  s.game_display_name = info.game_display_name;
  s.plugin_name       = info.plugin_name;
  s.author            = info.author;
  s.version           = info.version;
  s.description       = info.description;
  s.category          = info.category;
  s.nexus_domain      = info.nexus_domain;
  s.gog_id            = info.gog_id;
  s.epic_namespace    = info.epic_namespace;
  s.exe_windows       = info.exe_windows;
  s.exe_linux         = info.exe_linux;
  s.exe_macos         = info.exe_macos;
  s.steam_appid       = info.steam_appid;
  s.abi_version       = info.abi_version;
  s.features          = info.features;
  s.game_support      = info.game_support;
  s.registered        = info.registered;
  s.loaded            = info.loaded;
  s.settings          = info.settings;

  const auto &knowledge = loader.knowledge();
  for (const auto &key : knowledge.keys_for(info.game_id))
    s.knowledge.emplace_back(key, knowledge.get(info.game_id, key));
  std::sort(s.knowledge.begin(), s.knowledge.end());

  // The engine hook registry rows ride the same tags the knowledge rows use.
  for (const auto &key : knowledge.keys_for(info.game_id)) {
    for (const auto &h : loader.hook_registry().hooks_for(key))
      s.hooks.emplace_back(h.tag, h.priority, h.plugin_id);
  }
  std::sort(s.hooks.begin(), s.hooks.end());

  for (const auto &c : loader.capabilities().capabilities_for(info.game_id)) {
    s.capabilities.push_back(c.capability + "|" + c.display_name + "|" + c.data_path +
                             "|" + c.description + "|" + c.protocol_handler + "|" +
                             c.website_domain + "|" + c.insert_before + "|" +
                             c.insert_after);
  }
  std::sort(s.capabilities.begin(), s.capabilities.end());

  for (const char *gid : {"SkyrimSpecialEdition", "skyrim", "skyrimse", "skyrimvr"})
    if (SaveParserRegistry::instance().has_parser(gid))
      s.save_parsers.push_back(gid);

  for (const auto &v : info.variants)
    s.variants.push_back(v.variant_id + "|" + v.display_name);
  std::sort(s.variants.begin(), s.variants.end());

  if (info.save_overlay_fn != nullptr) {
    // The overlay fn is registered per game_id; both routes pass the same
    // two gids, so record which gids got a non-null fn.
    for (const char *gid : {"SkyrimSpecialEdition", "skyrimse"})
      s.save_overlays.push_back(gid);
  }

  if (const auto tool = PluginToolRegistry::instance().find("loot")) {
    s.tool = tool->tool_id + "|" + tool->kind + "|" + tool->plugin_path +
             (tool->fn != nullptr ? "|fn" : "|nofn") +
             (tool->user_data != nullptr ? "|ud" : "|noud");
  }

  if (const auto *provider =
          OrderEncodingRegistry::instance().get_provider(info.game_id)) {
    s.order_encoding = provider->game_id + "|" + provider->plugin_path +
                       (provider->fn != nullptr ? "|fn" : "|nofn") +
                       (provider->user_data != nullptr ? "|ud" : "|noud");
  }

  return s;
}

// Route B: replay the Skyrim plugin's gmm_register_v2 body straight onto a
// RegistrationSink (the v3 P0 path). Data mirrors
// Plugins/src/plugins/GameSupport/SkyrimSpecialEdition/SkyrimSpecialEdition.cpp.
SaveGame trivial_save_parser(const fs::path &, const std::string &) {
  return SaveGame{};
}

GmmSaveOverlayV2 *trivial_save_overlay(const GmmSaveDataV2 *, void *) {
  return nullptr;
}

int trivial_order_encoding(const char *const *, size_t, const char *, void *) {
  return 1;
}

Snapshot replay_route_b() {
  PluginInfo info;
  info.path              = GMM_SKYRIM_PLUGIN_PATH;
  info.game_id           = fs::path(GMM_SKYRIM_PLUGIN_PATH).stem().string();
  info.game_display_name = info.game_id;  // fallback, overridden by register_game
  info.abi_version       = 2;
  info.features          = (1ull << 0) | (1ull << 1) | (1ull << 3) | (1ull << 4);
  info.loaded            = true;

  PluginLoader loader;
  RegistrationSink sink(loader, info);

  GameDescriptor desc;
  desc.provenance   = DescriptorProvenance::CompiledV2;
  desc.plugin_path  = info.path;
  desc.id           = "SkyrimSpecialEdition";
  desc.display_name = "Skyrim Special Edition";
  desc.steam_appid  = 489830;
  desc.nexus_domain = "skyrimspecialedition";
  sink.add_entity(desc, "SkyrimSpecialEdition");

  sink.plugin_meta("Skyrim Special Edition", "GameModManager Team",
                   GMM_SKYRIM_PLUGIN_VERSION,
                   "Skyrim Special Edition game support (plugins.txt load "
                   "order, Data/ layout)");
  sink.category_register("Game Support");
  sink.order_encoding_register(reinterpret_cast<void *>(trivial_order_encoding),
                               nullptr);

  // Tabs - UI layout (verbatim from the plugin's .tabs({...}) call).
  sink.capability_register("plugins", "Plugins", "Data/",
                           "ESP/ESM/ESL plugin load order via plugins.txt", nullptr,
                           nullptr, nullptr, "archives", nullptr);
  sink.capability_register("archives", "Archives", "Data/",
                           "BSA/BA2 archive files loaded by the game engine", nullptr,
                           nullptr, nullptr, "downloads", nullptr);
  sink.capability_register(
      "saves", "Saves", "Documents/My Games/Skyrim Special Edition/Saves/",
      "Save game files (.ess, .skse)", nullptr, nullptr, nullptr, "downloads", "data");
  sink.capability_register("downloads", "Downloads", "downloads/",
                           "Download mods from Nexus Mods", "nxm", "nexusmods.com",
                           "nexus", nullptr, nullptr);
  sink.capability_register("data", "Data", "",
                           "Virtual file system view of the game's Data folder",
                           nullptr, nullptr, nullptr, "downloads", "archives");

  // Hooks - game-dependent data (data-only: fn = nullptr, priority = 0).
  const std::pair<const char *, const char *> hooks[] = {
      {"download_sources", "Nexus Mods,LoversLab"},
      {"mod_counter_label", "Plugins"},
      {"game_native_plugins", "Skyrim.esm,Update.esm,Dawnguard.esm,HearthFires.esm,"
                              "Dragonborn.esm,_ResourcePack.esl"},
      {"mods_subpath", "Data"},
      {"mod_valid_dirs", "fonts,interface,menus,meshes,music,scripts,shaders,sound,"
                         "strings,textures,trees,video,facegen,materials,skse,obse,"
                         "mwse,nvse,fose,f4se,distantlod,asi,SkyProc "
                         "Patchers,Tools,MCM,icons,bookart,distantland,mits,splash,"
                         "dllplugins,CalienteTools,NetScriptFramework,shadersfx"},
      {"mod_valid_exts", "esp,esm,esl,bsa,ba2,modgroups,ini"},
      {"case_sensitive", "false"},
      {"localappdata_folder", "Skyrim Special Edition"},
      {"mygames_folder", "Skyrim Special Edition"},
      {"executables", "SkyrimSE.exe,SkyrimSELauncher.exe,skse64_loader.exe"},
      {"steam_appid", "489830"},
      {"game_icon_url", "https://cdn2.steamgriddb.com/icon/"
                        "20d749bc05f47d2bd3026ce457dcfd8e/32/64x64.png"},
      {"loot_game_id", "skyrimse"},
      {"loot_masterlist_repo", "skyrimse"},
      {"save_fast_format", "gamebryo-tesv"},
  };
  for (const auto &hook : hooks) {
    sink.knowledge_set("SkyrimSpecialEdition", hook.first, hook.second);
    sink.hook_register(hook.first, hook.second, nullptr, 0, nullptr);
    sink.hook_register_v2(hook.first, nullptr, 0, nullptr, info.path.c_str());
  }

  sink.tool_register("loot", "advisory", nullptr, nullptr, true);

  for (const char *gid : {"SkyrimSpecialEdition", "skyrim", "skyrimse", "skyrimvr"})
    sink.save_parser_register(gid, trivial_save_parser, 0, nullptr);

  sink.save_overlay_register("SkyrimSpecialEdition",
                             reinterpret_cast<void *>(trivial_save_overlay), 0,
                             nullptr);
  sink.save_overlay_register("skyrimse", reinterpret_cast<void *>(trivial_save_overlay),
                             0, nullptr);

  sink.game_variant_register("SkyrimSpecialEdition", "steam", "Steam");
  sink.game_variant_register("SkyrimSpecialEdition", "gog", "GOG");
  sink.game_variant_register("SkyrimSpecialEdition", "epic", "Epic Games Store");
  sink.game_variant_register("skyrimse", "steam", "Steam");
  sink.game_variant_register("skyrimse", "gog", "GOG");
  sink.game_variant_register("skyrimse", "epic", "Epic Games Store");

  info.registered = true;

  Snapshot s = snapshot(loader, info);
  // loader's destructor (unload_all) must not run before the snapshot is
  // taken - it already has been. Keep the loader alive to the end of the
  // scope; the snapshot copies everything it needs.
  return s;
}

void compare(const Snapshot &a, const Snapshot &b) {
  check(a.path == b.path, "path");
  check(a.game_id == b.game_id, "game_id: A=" + a.game_id + " B=" + b.game_id);
  check(a.game_display_name == b.game_display_name, "game_display_name");
  check(a.plugin_name == b.plugin_name, "plugin_name");
  check(a.author == b.author, "author");
  check(a.version == b.version, "version");
  check(a.description == b.description, "description");
  check(a.category == b.category, "category");
  check(a.nexus_domain == b.nexus_domain, "nexus_domain");
  check(a.gog_id == b.gog_id, "gog_id");
  check(a.epic_namespace == b.epic_namespace, "epic_namespace");
  check(a.exe_windows == b.exe_windows, "exe_windows");
  check(a.exe_linux == b.exe_linux, "exe_linux");
  check(a.exe_macos == b.exe_macos, "exe_macos");
  check(a.steam_appid == b.steam_appid, "steam_appid");
  check(a.abi_version == b.abi_version, "abi_version");
  check(a.features == b.features, "features");
  check(a.game_support == b.game_support, "game_support");
  check(a.registered == b.registered, "registered");
  check(a.loaded == b.loaded, "loaded");
  check(a.settings == b.settings, "settings");

  check(a.knowledge.size() == b.knowledge.size(), "knowledge row count");
  for (size_t i = 0; i < a.knowledge.size() && i < b.knowledge.size(); ++i) {
    check(a.knowledge[i] == b.knowledge[i],
          "knowledge row " + std::to_string(i) + ": A=(" + a.knowledge[i].first + "," +
              a.knowledge[i].second + ") B=(" + b.knowledge[i].first + "," +
              b.knowledge[i].second + ")");
  }

  check(a.hooks.size() == b.hooks.size(), "hook row count");
  for (size_t i = 0; i < a.hooks.size() && i < b.hooks.size(); ++i) {
    check(a.hooks[i] == b.hooks[i], "hook row " + std::to_string(i));
  }

  check(a.capabilities.size() == b.capabilities.size(), "capability count");
  for (size_t i = 0; i < a.capabilities.size() && i < b.capabilities.size(); ++i) {
    check(a.capabilities[i] == b.capabilities[i], "capability " + std::to_string(i) +
                                                      ": A=" + a.capabilities[i] +
                                                      " B=" + b.capabilities[i]);
  }

  check(a.save_parsers == b.save_parsers, "save parsers");
  check(a.save_overlays == b.save_overlays, "save overlays");
  check(a.variants == b.variants, "game variants");
  check(a.tool == b.tool, "tool: A=" + a.tool + " B=" + b.tool);
  check(a.order_encoding == b.order_encoding,
        "order encoding: A=" + a.order_encoding + " B=" + b.order_encoding);
}

}  // namespace

TEST_CASE("RegistrationSink parity: ABI bridge route == direct sink replay",
          "[registration_sink]") {
  // Route A: the real Skyrim plugin through the (rewritten) ABI handlers.
  Snapshot a;
  {
    PluginLoader loader_a;
    check(loader_a.load_plugin(GMM_SKYRIM_PLUGIN_PATH),
          "Skyrim plugin failed to load through the ABI bridge");
    check(loader_a.plugins().size() == 1, "exactly one plugin loaded");
    a = snapshot(loader_a, loader_a.plugins().front());
  }  // unload_all: drops the plugin's rows from the process-wide registries

  // Route B: the same registration replayed straight onto a sink.
  const Snapshot b = replay_route_b();

  compare(a, b);

  // Sanity: route A must actually have exercised the paths under test, or the
  // comparison above is 15 empty rows agreeing with 15 empty rows.
  check(a.knowledge.size() == 15, "15 knowledge rows (route A)");
  check(a.capabilities.size() == 5, "5 capability tabs (route A)");
  check(a.save_parsers.size() == 4, "4 save parsers (route A)");
  check(a.variants.size() == 6, "6 game variants (route A)");
  check(a.game_support, "game_support set (route A)");
  check(!a.tool.empty(), "loot tool registered (route A)");
  check(!a.order_encoding.empty(), "order encoding provider (route A)");
}
