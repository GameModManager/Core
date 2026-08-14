// P1.2 GameFeatureRegistry test — MO2's IGameFeatures analogue (PLAN.md §19.4
// P1.2). Pins:
//   - priority + replace registration: the game plugin's own feature is the
//     lowest-priority baseline; a higher-priority registration supersedes it
//     for resolve(); equal priority = last registered wins,
//   - the combined ModDataChecker (MO2 CombinedModDataChecker): ALL registered
//     checkers' allow-sets union — ANY checker VALID -> VALID — and that union
//     drives the mod list's FLAG_INVALID ("No valid game data"),
//   - the knowledge-hook fallback (mod_valid_dirs/mod_valid_exts) survives for
//     games whose plugin still uses it and for scanner knowledge tests,
//   - GamePlugins (MO2 GamePlugins::gamePlugins): the registered game_plugins
//     feature replaces the game_native_plugins hook via native_plugins_csv()
//     (registry-first, hook fallback),
//   - the seven structured-data feature types (mod_data_content,
//     data_archives, script_extender, save_game_info, local_savegames,
//     unmanaged_mods, bsa_invalidation): register_game_feature_data kv parse,
//     resolve_feature<T>() resolution, priority replace, per-game isolation,
//     and the ModDataContentFeature classifier (a data-driven port of MO2's
//     gamebryomoddatacontent.cpp contents detection),
//   - the P1.2 exit criterion end-to-end: a TEST PLUGIN overrides the Skyrim
//     plugin's data-checker, vanilla-plugin band, AND script extender via the
//     register_game_feature / register_game_feature_data C ABI, and the mod
//     list (ModScanner -> invalid_data) / native_plugins_csv show the
//     override — engine untouched.
//
// Uses the check() PASS/FAIL pattern (Release builds compile out assert()).

#include "engine/game/detect/mod_scanner.h"
#include "engine/core/events/event_bus.h"
#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "engine/game/registry/game_features/game_feature_registry.h"
#include "engine/game/registry/game_knowledge.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

#ifndef GMM_OVERRIDE_PLUGIN_PATH
#define GMM_OVERRIDE_PLUGIN_PATH "gmm_override_mod_data_checker.so"
#endif
#ifndef GMM_SKYRIM_PLUGIN_PATH
#define GMM_SKYRIM_PLUGIN_PATH "SkyrimSpecialEdition.so"
#endif


namespace {
void check(bool cond, const std::string& msg) {
    INFO(msg);
    REQUIRE(cond);
}
}

static void write_file(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    out << contents;
    check(out.good(), "write_file failed for " + p.string());
}

static const engine::ScannedMod* by_folder(const std::vector<engine::ScannedMod>& mods,
                                           const std::string& folder) {
    for (const auto& m : mods)
        if (m.folder_name == folder) return &m;
    return nullptr;
}

static std::shared_ptr<engine::ModDataCheckerFeature> make_checker(
    std::vector<std::string> folders, std::vector<std::string> exts) {
    return std::make_shared<engine::ModDataCheckerFeature>(
        std::move(folders), std::move(exts));
}

static void test_registry_semantics() {
    std::printf("=== test_registry_semantics ===\n");
    auto& reg = engine::GameFeatureRegistry::instance();
    reg.clear();

    check(reg.resolve("skyrim", "mod_data_checker") == nullptr,
          "empty registry resolves nullptr");

    // Game plugin's own feature at the lowest priority.
    reg.register_feature("skyrim", "mod_data_checker", 0,
        make_checker({"textures", "meshes"}, {"esp", "esm"}), "skyrim_plugin");
    check(reg.resolve("skyrim", "mod_data_checker") != nullptr,
          "registered feature resolves");

    // A higher-priority registration overrides it for resolve() (priority +
    // replace), and equal priority = last registered wins.
    reg.register_feature("skyrim", "mod_data_checker", 10,
        make_checker({"customstuff"}, {"custoext"}), "override_plugin");
    reg.register_feature("skyrim", "mod_data_checker", 10,
        make_checker({"secondstuff"}, {}), "second_plugin");
    auto winner = std::dynamic_pointer_cast<engine::ModDataCheckerFeature>(
        reg.resolve("skyrim", "mod_data_checker"));
    check(winner != nullptr, "resolve returns the override feature");
    check(winner && winner->folder_names().size() == 1 &&
          winner->folder_names()[0] == "secondstuff",
          "equal priority: last registered wins");

    // Per-game isolation.
    check(reg.resolve("isaac", "mod_data_checker") == nullptr,
          "another game resolves nullptr");
    reg.register_feature("isaac", "mod_data_checker", 0,
        make_checker({"resources"}, {}), "isaac_plugin");
    check(reg.resolve("isaac", "mod_data_checker") != nullptr,
          "isaac's own checker resolves");
    check(reg.resolve("skyrim", "mod_data_checker") != nullptr,
          "skyrim resolve unaffected by isaac registration");

    // Combined ModDataChecker = union of ALL registered checkers' allow-sets.
    auto combined = reg.resolve_mod_data_checker("skyrim");
    check(combined != nullptr, "combined skyrim checker resolves");
    check(combined && combined->folder_names().size() == 4,
          "union keeps textures, meshes, customstuff, secondstuff");
    bool has_tex = false, has_custom = false;
    if (combined) {
        for (const auto& d : combined->folder_names()) {
            if (d == "textures") has_tex = true;
            if (d == "customstuff") has_custom = true;
        }
    }
    check(has_tex, "game's own baseline folder still in the union");
    check(has_custom, "override folder in the union");

    // features_for exposes every registration (order preserved).
    auto all = reg.features_for("skyrim", "mod_data_checker");
    check(all.size() == 3, "features_for returns all 3 skyrim registrations");

    // GamePlugins (MO2 GamePlugins::gamePlugins): same priority + replace
    // semantics, own type key. Game's own band at priority 0, an override at
    // higher priority wins resolve().
    check(reg.resolve_game_plugins("skyrim") == nullptr,
          "no game_plugins registered yet -> nullptr");
    reg.register_feature("skyrim", "game_plugins", 0,
        std::make_shared<engine::GamePluginsFeature>(
            std::vector<std::string>{"Skyrim.esm", "Update.esm"}), "skyrim_plugin");
    auto own_band = reg.resolve_game_plugins("skyrim");
    check(own_band != nullptr && own_band->plugins().size() == 2 &&
          own_band->plugins()[0] == "Skyrim.esm",
          "game's own game_plugins band resolves");
    reg.register_feature("skyrim", "game_plugins", 50,
        std::make_shared<engine::GamePluginsFeature>(
            std::vector<std::string>{"Override.esm"}), "override_plugin");
    auto over_band = reg.resolve_game_plugins("skyrim");
    check(over_band != nullptr && over_band->plugins().size() == 1 &&
          over_band->plugins()[0] == "Override.esm",
          "higher-priority game_plugins override wins resolve");
    check(reg.resolve_game_plugins("isaac") == nullptr,
          "game_plugins per-game isolation");

    // Clear drops everything.
    reg.clear();
    check(reg.resolve("skyrim", "mod_data_checker") == nullptr,
          "clear drops registrations");
    check(reg.resolve_mod_data_checker("isaac") == nullptr,
          "clear drops combined checkers");
    check(reg.resolve_game_plugins("skyrim") == nullptr,
          "clear drops game_plugins registrations");
}

static void test_scanner_integration() {
    std::printf("=== test_scanner_integration ===\n");
    auto& reg = engine::GameFeatureRegistry::instance();
    reg.clear();

    const fs::path root = "/tmp/gmm_feature_registry_test";
    fs::remove_all(root);
    fs::create_directories(root / "CustomMod" / "customstuff");
    fs::create_directories(root / "OtherMod" / "otherstuff");

    // Knowledge-hook fallback survives: no feature registered -> the per-game
    // CSV allow-lists drive FLAG_INVALID exactly as before.
    engine::GameKnowledge knowledge;
    knowledge.set("skyrim", "mod_valid_dirs", "textures,meshes");
    knowledge.set("skyrim", "mod_valid_exts", "esp,esm");
    fs::create_directories(root / "FallbackBad");
    fs::create_directories(root / "FallbackGood" / "textures");
    auto fb = engine::ModScanner::scan_dir(knowledge, "skyrim", root);
    const auto* fbb = by_folder(fb, "FallbackBad");
    const auto* fbg = by_folder(fb, "FallbackGood");
    check(fbb && fbb->invalid_data, "knowledge fallback: empty folder invalid");
    check(fbg && !fbg->invalid_data, "knowledge fallback: textures/ folder valid");

    // Registry-driven: a registered checker (no knowledge at all) drives the
    // same flag. customstuff/ is valid, otherstuff/ is not.
    reg.register_feature("skyrim", "mod_data_checker", 0,
        make_checker({"customstuff"}, {}), "checker_plugin");
    auto mods = engine::ModScanner::scan_dir(engine::GameKnowledge{}, "skyrim", root);
    const auto* cm = by_folder(mods, "CustomMod");
    const auto* om = by_folder(mods, "OtherMod");
    check(cm != nullptr && !cm->invalid_data,
          "registered checker accepts customstuff/ (not invalid)");
    check(om != nullptr && om->invalid_data,
          "registered checker flags otherstuff/ as no valid game data");

    reg.clear();
}

static void test_native_plugins_resolution() {
    std::printf("=== test_native_plugins_resolution ===\n");
    auto& reg = engine::GameFeatureRegistry::instance();
    reg.clear();

    engine::GameKnowledge knowledge;
    knowledge.set("skyrim", "game_native_plugins",
                  "Skyrim.esm,Update.esm,Dawnguard.esm");

    // Knowledge fallback: no registered feature -> the hook CSV drives the
    // consumers (plugin_database / mod list / mod scan) exactly as before.
    check(engine::native_plugins_csv(knowledge, "skyrim") ==
              "Skyrim.esm,Update.esm,Dawnguard.esm",
          "knowledge fallback: hook CSV returned verbatim");

    // Registry wins: a registered game_plugins feature replaces the hook.
    reg.register_feature("skyrim", "game_plugins", 0,
        std::make_shared<engine::GamePluginsFeature>(
            std::vector<std::string>{"Custom.esm", "Other.esm"}), "skyrim_plugin");
    check(engine::native_plugins_csv(knowledge, "skyrim") == "Custom.esm,Other.esm",
          "registered game_plugins feature supersedes the hook");

    // A higher-priority registration overrides it.
    reg.register_feature("skyrim", "game_plugins", 10,
        std::make_shared<engine::GamePluginsFeature>(
            std::vector<std::string>{"Override.esm"}), "override_plugin");
    check(engine::native_plugins_csv(knowledge, "skyrim") == "Override.esm",
          "higher-priority game_plugins overrides for the CSV");

    // Per-game isolation + empty when nothing declares a band.
    check(engine::native_plugins_csv(knowledge, "isaac") == "",
          "game with no native plugin source resolves empty");
    reg.register_feature("isaac", "game_plugins", 0,
        std::make_shared<engine::GamePluginsFeature>(
            std::vector<std::string>{"Isaac.esm"}), "isaac_plugin");
    check(engine::native_plugins_csv(knowledge, "skyrim") == "Override.esm",
          "isaac registration does not leak into skyrim");

    reg.clear();
}

static void test_all_feature_types() {
    std::printf("=== test_all_feature_types ===\n");
    auto& reg = engine::GameFeatureRegistry::instance();
    reg.clear();

    // Empty/unknown registrations are refused (logged).
    check(!engine::register_game_feature_data("", "script_extender", 0, {}, ""),
          "empty game_id refused");
    check(!engine::register_game_feature_data("skyrim", "", 0, {}, ""),
          "empty feature_type refused");
    check(!engine::register_game_feature_data("skyrim", "bogus", 0, {}, ""),
          "unknown feature_type refused");

    // data_archives: the vanilla archive list (csv split).
    check(engine::register_game_feature_data("skyrim", "data_archives", 0,
              {{"vanilla_archives", "a.bsa, b.bsa"}}, "skyrim_plugin"),
          "data_archives registers");
    auto da = reg.resolve_feature<engine::DataArchivesFeature>("skyrim");
    check(da && da->vanilla_archives().size() == 2 &&
          da->vanilla_archives()[0] == "a.bsa" &&
          da->vanilla_archives()[1] == "b.bsa",
          "data_archives resolves the csv split list");

    // script_extender: four named fields + priority replace.
    check(engine::register_game_feature_data("skyrim", "script_extender", 0,
              {{"binary", "skse64_loader.exe"}, {"plugin_path", "skse/plugins"},
               {"loader_name", "skse64_loader.exe"},
               {"savegame_extension", "skse"}}, "skyrim_plugin"),
          "script_extender registers");
    auto se = reg.resolve_feature<engine::ScriptExtenderFeature>("skyrim");
    check(se && se->binary_name() == "skse64_loader.exe" &&
          se->plugin_path() == "skse/plugins" &&
          se->loader_name() == "skse64_loader.exe" &&
          se->savegame_extension() == "skse",
          "script_extender resolves all four fields");
    check(engine::register_game_feature_data("skyrim", "script_extender", 10,
              {{"binary", "other.exe"}, {"plugin_path", "other/plugins"},
               {"loader_name", "other.exe"}, {"savegame_extension", "xse"}},
              "override_plugin"),
          "script_extender override registers");
    auto se2 = reg.resolve_feature<engine::ScriptExtenderFeature>("skyrim");
    check(se2 && se2->binary_name() == "other.exe" &&
          se2->savegame_extension() == "xse",
          "higher-priority script_extender overrides");

    // save_game_info: extension csv.
    check(engine::register_game_feature_data("skyrim", "save_game_info", 0,
              {{"extensions", "ess,skse"}}, "skyrim_plugin"),
          "save_game_info registers");
    auto sgi = reg.resolve_feature<engine::SaveGameInfoFeature>("skyrim");
    check(sgi && sgi->savegame_extensions().size() == 2 &&
          sgi->savegame_extensions()[0] == "ess" &&
          sgi->savegame_extensions()[1] == "skse",
          "save_game_info resolves the extension list");

    // local_savegames: subpath + ini.
    check(engine::register_game_feature_data("skyrim", "local_savegames", 0,
              {{"saves_subpath", "Saves"}, {"ini_file", "Skyrimcustom.ini"}},
              "skyrim_plugin"),
          "local_savegames registers");
    auto lsg = reg.resolve_feature<engine::LocalSavegamesFeature>("skyrim");
    check(lsg && lsg->saves_subpath() == "Saves" &&
          lsg->ini_file() == "Skyrimcustom.ini",
          "local_savegames resolves subpath + ini");

    // unmanaged_mods + the consumer helper.
    check(engine::register_game_feature_data("skyrim", "unmanaged_mods", 0,
              {{"mods", "DLC: Dawnguard,CC: Foo"}}, "skyrim_plugin"),
          "unmanaged_mods registers");
    auto um = reg.resolve_feature<engine::UnmanagedModsFeature>("skyrim");
    check(um && um->mods().size() == 2 && um->mods()[0] == "DLC: Dawnguard",
          "unmanaged_mods resolves the mod names");
    check(engine::unmanaged_mods_for("skyrim").size() == 2 &&
          engine::unmanaged_mods_for("skyrim")[1] == "CC: Foo",
          "unmanaged_mods_for returns the registered names");
    check(engine::unmanaged_mods_for("isaac").empty(),
          "unmanaged_mods_for empty for a game without the feature");

    // bsa_invalidation: name + version.
    check(engine::register_game_feature_data("skyrim", "bsa_invalidation", 0,
              {{"bsa_name", "Skyrim - Invalidation.bsa"}, {"bsa_version", "0x68"}},
              "skyrim_plugin"),
          "bsa_invalidation registers");
    auto bsa = reg.resolve_feature<engine::BSAInvalidationFeature>("skyrim");
    check(bsa && bsa->bsa_name() == "Skyrim - Invalidation.bsa" &&
          bsa->bsa_version() == "0x68",
          "bsa_invalidation resolves name + version");

    // mod_data_content: enabled catalog ids + content: overrides.
    check(engine::register_game_feature_data("skyrim", "mod_data_content", 0,
              {{"enabled", "plugin,texture"}, {"content:plugin", "Plugins|plugin2|0"}},
              "skyrim_plugin"),
          "mod_data_content registers");
    auto mdc = reg.resolve_feature<engine::ModDataContentFeature>("skyrim");
    check(mdc != nullptr && mdc->enabled_ids().size() == 2,
          "mod_data_content enabled list parsed");
    std::vector<engine::ModDataContentFeature::Content> contents =
        mdc ? mdc->all_contents() : std::vector<engine::ModDataContentFeature::Content>{};
    check(contents.size() == 2 && contents[0].name == "Plugins" &&
          contents[0].icon == "plugin2",
          "content: override applied to the catalog entry");
    check(contents.size() == 2 && contents[1].name == "Textures",
          "enabled filter keeps the second catalog entry");

    // Unknown catalog ids in enabled are dropped, not fatal.
    check(engine::register_game_feature_data("skyrim2", "mod_data_content", 0,
              {{"enabled", "plugin,bogus"}}, "p"),
          "mod_data_content tolerates unknown enabled ids");
    auto mdc2 = reg.resolve_feature<engine::ModDataContentFeature>("skyrim2");
    check(mdc2 && mdc2->enabled_ids().size() == 1 &&
          mdc2->enabled_ids()[0] == engine::ModContentId::Plugin,
          "unknown enabled id dropped");

    // Per-game isolation.
    check(reg.resolve_feature<engine::ScriptExtenderFeature>("isaac") == nullptr,
          "script_extender per-game isolation");
    check(reg.resolve_feature<engine::BSAInvalidationFeature>("isaac") == nullptr,
          "bsa_invalidation per-game isolation");

    reg.clear();
}

static void test_mod_data_content_classifier() {
    std::printf("=== test_mod_data_content_classifier ===\n");
    auto& reg = engine::GameFeatureRegistry::instance();
    reg.clear();

    const fs::path root = "/tmp/gmm_feature_content_test";
    fs::remove_all(root);
    fs::create_directories(root / "textures" / "armor");
    fs::create_directories(root / "scripts");
    fs::create_directories(root / "skse" / "plugins");
    fs::create_directories(root / "meshes" / "actors" / "character" / "facegendata");
    write_file(root / "Foo.esm", "");
    write_file(root / "skse" / "plugins" / "x.dll", "");
    write_file(root / "note.txt", "");

    auto tree = engine::FileTree::make_tree_from_directory(root);
    check(tree != nullptr, "file tree builds from the mod dir");

    const std::string all_enabled =
        "plugin,optional,interface,mesh,bsa,script,skse,skse_files,skyproc,"
        "sound,texture,mcm,ini,facegen,modgroup";
    check(engine::register_game_feature_data("skyrim", "mod_data_content", 0,
              {{"enabled", all_enabled}}, "p"),
          "all-content feature registers");

    auto has = [](const std::vector<int>& ids, int id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };

    auto mdc = reg.resolve_feature<engine::ModDataContentFeature>("skyrim");
    check(mdc != nullptr, "mod_data_content resolves");
    std::vector<int> ids = mdc ? mdc->contents_for(tree, "skse/plugins")
                               : std::vector<int>{};
    check(has(ids, engine::ModContentId::Plugin),
          "classifier finds the top-level esm");
    check(has(ids, engine::ModContentId::Texture),
          "classifier finds textures/");
    check(has(ids, engine::ModContentId::Script),
          "classifier finds scripts/");
    check(has(ids, engine::ModContentId::Mesh),
          "classifier finds meshes/");
    check(has(ids, engine::ModContentId::Facegen),
          "classifier finds meshes/.../facegendata");
    check(has(ids, engine::ModContentId::Skse),
          "classifier finds a dll under the plugin path");
    check(has(ids, engine::ModContentId::SkseFiles),
          "classifier finds the plugin-path dir");
    check(!has(ids, engine::ModContentId::Bsa) &&
          !has(ids, engine::ModContentId::Mcm) &&
          !has(ids, engine::ModContentId::Ini) &&
          !has(ids, engine::ModContentId::Optional),
          "classifier flags nothing for absent categories");

    // Disabled categories are not reported even when present: an override
    // with only plugin/texture/script enabled hides skse + facegen.
    check(engine::register_game_feature_data("skyrim", "mod_data_content", 10,
              {{"enabled", "plugin,texture,script"}}, "override"),
          "override registers");
    auto mdc2 = reg.resolve_feature<engine::ModDataContentFeature>("skyrim");
    std::vector<int> ids2 = mdc2 ? mdc2->contents_for(tree, "skse/plugins")
                                 : std::vector<int>{};
    check(!has(ids2, engine::ModContentId::Skse),
          "skse disabled -> not reported");
    check(!has(ids2, engine::ModContentId::SkseFiles),
          "skse_files disabled -> not reported");
    check(!has(ids2, engine::ModContentId::Facegen),
          "facegen disabled -> not reported");

    // Without a script-extender plugin path the skse dir is not classified.
    auto mdc3 = reg.resolve_feature<engine::ModDataContentFeature>("skyrim");
    std::vector<int> ids3 = mdc3 ? mdc3->contents_for(tree, "") : std::vector<int>{};
    check(!has(ids3, engine::ModContentId::Skse) &&
          !has(ids3, engine::ModContentId::SkseFiles),
          "no plugin path -> no skse categories");

    reg.clear();
}

static void test_override_via_c_abi() {
    std::printf("=== test_override_via_c_abi ===\n");
    auto& reg = engine::GameFeatureRegistry::instance();
    reg.clear();

    engine::PluginLoader loader;

    // The Skyrim plugin registers its own mod_data_checker at priority 0
    // (the game's own feature, the lowest baseline).
    check(loader.load_plugin(GMM_SKYRIM_PLUGIN_PATH),
          "Skyrim plugin loads");
    // A test plugin overrides it at priority 100 through the C ABI.
    check(loader.load_plugin(GMM_OVERRIDE_PLUGIN_PATH),
          "override test plugin loads");
    check(loader.plugins().size() == 2, "both plugins registered");

    auto all = reg.features_for("SkyrimSpecialEdition", "mod_data_checker");
    check(all.size() == 2, "both checkers registered for SkyrimSpecialEdition");
    check(all.size() == 2 && all[0].priority == 0 && all[1].priority == 100,
          "game's own at priority 0, override at priority 100");

    // The Skyrim plugin also registers its game_plugins band (the 6 vanilla
    // ESMs) at priority 0; the override replaces it at priority 100, and
    // native_plugins_csv() — the single source plugin_database / the mod list
    // / mod scan feed — returns the override's band.
    auto bands = reg.features_for("SkyrimSpecialEdition", "game_plugins");
    check(bands.size() == 2, "both game_plugins bands registered");
    check(bands.size() == 2 && bands[0].priority == 0 && bands[1].priority == 100,
          "game's own band at priority 0, override at priority 100");
    auto skyrim_band = std::dynamic_pointer_cast<const engine::GamePluginsFeature>(
        bands[0].feature);
    check(skyrim_band && skyrim_band->plugins().size() == 6 &&
          skyrim_band->plugins()[0] == "Skyrim.esm" &&
          skyrim_band->plugins()[5] == "_ResourcePack.esl",
          "Skyrim's own plugin registers its 6 vanilla plugins");
    auto gp_winner = reg.resolve_game_plugins("SkyrimSpecialEdition");
    check(gp_winner != nullptr && gp_winner->plugins().size() == 2 &&
          gp_winner->plugins()[0] == "VanillaOverride.esm",
          "resolve_game_plugins returns the override's band");

    const std::string native_csv =
        engine::native_plugins_csv(engine::GameKnowledge{}, "SkyrimSpecialEdition");
    check(native_csv == "VanillaOverride.esm,AlsoVanilla.esm",
          "native_plugins_csv resolves through the C ABI override");
    check(native_csv.find("Skyrim.esm") == std::string::npos,
          "override fully replaces Skyrim's own vanilla band");

    // The Skyrim plugin registers all 8 feature classes it has (every one of
    // MO2's set except bsa_invalidation, which Skyrim SE doesn't use) at
    // priority 0 with MO2's gamebryo values; the override fixture proves the
    // full ABI surface — it overrides script_extender at priority 100 and
    // adds bsa_invalidation, a type Skyrim's own plugin does not register.
    auto se = reg.resolve_feature<engine::ScriptExtenderFeature>("SkyrimSpecialEdition");
    check(se != nullptr && se->binary_name() == "superse_loader.exe" &&
          se->plugin_path() == "superse/plugins" &&
          se->loader_name() == "superse_loader.exe" &&
          se->savegame_extension() == "sse",
          "script_extender resolves the C-ABI override");
    auto se_regs = reg.features_for("SkyrimSpecialEdition", "script_extender");
    check(se_regs.size() == 2 && se_regs[0].priority == 0 && se_regs[1].priority == 100,
          "Skyrim's own script_extender at priority 0, override at 100");
    auto own_se = std::dynamic_pointer_cast<const engine::ScriptExtenderFeature>(
        se_regs[0].feature);
    check(own_se && own_se->binary_name() == "skse64_loader.exe" &&
          own_se->plugin_path() == "skse/plugins" &&
          own_se->loader_name() == "skse64_loader.exe" &&
          own_se->savegame_extension() == "skse",
          "Skyrim's own script_extender carries MO2's gamebryo values");
    auto bsa =
        reg.resolve_feature<engine::BSAInvalidationFeature>("SkyrimSpecialEdition");
    check(bsa != nullptr && bsa->bsa_name() == "CustomInvalidation.bsa" &&
          bsa->bsa_version() == "0x68",
          "bsa_invalidation registers via the C ABI though Skyrim registers none");
    check(reg.features_for("SkyrimSpecialEdition", "bsa_invalidation").size() == 1,
          "exactly one bsa_invalidation registration (the override)");

    auto da = reg.resolve_feature<engine::DataArchivesFeature>("SkyrimSpecialEdition");
    check(da != nullptr && da->vanilla_archives().size() == 17 &&
          da->vanilla_archives()[0] == "Skyrim - Textures0.bsa" &&
          da->vanilla_archives()[16] == "Skyrim - Misc.bsa",
          "data_archives carries Skyrim's 17 vanilla archives");
    auto sgi =
        reg.resolve_feature<engine::SaveGameInfoFeature>("SkyrimSpecialEdition");
    check(sgi != nullptr && sgi->savegame_extensions().size() == 2 &&
          sgi->savegame_extensions()[0] == "ess" &&
          sgi->savegame_extensions()[1] == "skse",
          "save_game_info carries ess + skse");
    auto lsg =
        reg.resolve_feature<engine::LocalSavegamesFeature>("SkyrimSpecialEdition");
    check(lsg != nullptr && lsg->saves_subpath() == "Saves" &&
          lsg->ini_file() == "Skyrimcustom.ini",
          "local_savegames carries Saves + Skyrimcustom.ini");
    auto mdc =
        reg.resolve_feature<engine::ModDataContentFeature>("SkyrimSpecialEdition");
    check(mdc != nullptr && mdc->all_contents().size() == 14,
          "mod_data_content: 14 categories (15 minus SKYPROC)");
    bool has_skyproc = false;
    if (mdc) {
        for (const auto& c : mdc->all_contents())
            if (c.name == "SkyProc Patcher") has_skyproc = true;
    }
    check(!has_skyproc, "mod_data_content: SKYPROC disabled for Skyrim SE");
    auto um = reg.resolve_feature<engine::UnmanagedModsFeature>("SkyrimSpecialEdition");
    check(um != nullptr && um->mods().empty(),
          "unmanaged_mods feature present but empty (strays come from the scan)");

    auto combined = reg.resolve_mod_data_checker("SkyrimSpecialEdition");
    check(combined != nullptr, "combined checker resolves");
    bool has_tex = false, has_custom = false, has_ext = false;
    if (combined) {
        for (const auto& d : combined->folder_names()) {
            if (d == "textures") has_tex = true;
            if (d == "customstuff") has_custom = true;
        }
        for (const auto& e : combined->file_extensions()) {
            if (e == "esp") has_ext = true;
        }
    }
    check(has_tex, "Skyrim's own baseline folder still accepted");
    check(has_custom, "override folder accepted (override visible)");
    check(has_ext, "Skyrim's own extensions still accepted");

    // The mod list (ModScanner -> ScannedMod::invalid_data -> mod_list_model
    // FLAG_INVALID) shows the override: a mod whose only content is the
    // override's customstuff/ is no longer "No valid game data", textures/
    // stays valid (game's own baseline), otherstuff/ stays invalid.
    const fs::path root = "/tmp/gmm_feature_registry_abi_test";
    fs::remove_all(root);
    fs::create_directories(root / "OverrideMod" / "customstuff");
    fs::create_directories(root / "BaseMod" / "textures");
    fs::create_directories(root / "ForeignMod" / "otherstuff");
    // The scanner's GameKnowledge is the TEST's, not the loader's: the registry
    // (populated via the plugins' C ABI calls) is what decides validity.
    auto mods = engine::ModScanner::scan_dir(
        engine::GameKnowledge{}, "SkyrimSpecialEdition", root);
    const auto* override_mod = by_folder(mods, "OverrideMod");
    const auto* base_mod = by_folder(mods, "BaseMod");
    const auto* foreign_mod = by_folder(mods, "ForeignMod");
    check(override_mod != nullptr && !override_mod->invalid_data,
          "mod list shows the override: customstuff/ mod is valid content");
    check(base_mod != nullptr && !base_mod->invalid_data,
          "mod list keeps Skyrim's own textures/ as valid content");
    check(foreign_mod != nullptr && foreign_mod->invalid_data,
          "mod list flags otherstuff/ as no valid game data");

    // P1.3 — the C ABI subscribe_event path end-to-end: the override fixture
    // subscribed to mod_installed + game_finished during register(); driving
    // the same public bus the UI emits into must reach the fixture's handler
    // (which logs to GMM_TEST_EVENTS_LOG).
    const fs::path ev_log = "/tmp/gmm_feature_registry_abi_events.log";
    std::error_code ev_ec;
    fs::remove(ev_log, ev_ec);
    ::setenv("GMM_TEST_EVENTS_LOG", ev_log.c_str(), 1);
    engine::EventBus::instance().dispatch(
        engine::events::kModInstalled,
        engine::json_obj({{"mod", "SkyUI"}, {"name", "SkyUI"}}));
    engine::EventBus::instance().dispatch(
        engine::events::kGameFinished,
        engine::json_obj({{"exit_code", "0"}}));
    std::ifstream evf(ev_log);
    std::vector<std::string> ev_lines;
    std::string ev_line;
    while (std::getline(evf, ev_line)) ev_lines.push_back(ev_line);
    check(ev_lines.size() == 2,
          "C-ABI fixture received both dispatched events");
    check(ev_lines.size() == 2 &&
          ev_lines[0] == "mod_installed {\"mod\":\"SkyUI\",\"name\":\"SkyUI\"}",
          "C-ABI fixture logged mod_installed payload verbatim");
    check(ev_lines.size() == 2 &&
          ev_lines[1] == "game_finished {\"exit_code\":\"0\"}",
          "C-ABI fixture logged game_finished payload verbatim");
    engine::EventBus::instance().clear();

    reg.clear();
}

TEST_CASE("game feature registry", "[engine]") {
    test_registry_semantics();
    test_scanner_integration();
    test_native_plugins_resolution();
    test_all_feature_types();
    test_mod_data_content_classifier();
    test_override_via_c_abi();

}
