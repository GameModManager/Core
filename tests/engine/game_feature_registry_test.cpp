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
//   - the P1.2 exit criterion end-to-end: a TEST PLUGIN overrides the Skyrim
//     plugin's data-checker via the register_game_feature C ABI, and the mod
//     list (ModScanner -> invalid_data) shows the override — engine untouched.
//
// Uses the check() PASS/FAIL pattern (Release builds compile out assert()).

#include "engine/detect/mod_scanner.h"
#include "engine/plugin_host/plugin_loader.h"
#include "engine/registry/game_features/game_feature_registry.h"
#include "engine/registry/game_knowledge.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

#ifndef GMM_OVERRIDE_PLUGIN_PATH
#define GMM_OVERRIDE_PLUGIN_PATH "gmm_override_mod_data_checker.so"
#endif
#ifndef GMM_SKYRIM_PLUGIN_PATH
#define GMM_SKYRIM_PLUGIN_PATH "SkyrimSpecialEdition.so"
#endif

static int g_failed = 0;
static int g_checked = 0;

static void check(bool cond, const std::string& msg) {
    ++g_checked;
    if (!cond) {
        ++g_failed;
        std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
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

    // Clear drops everything.
    reg.clear();
    check(reg.resolve("skyrim", "mod_data_checker") == nullptr,
          "clear drops registrations");
    check(reg.resolve_mod_data_checker("isaac") == nullptr,
          "clear drops combined checkers");
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

    reg.clear();
}

int main() {
    test_registry_semantics();
    test_scanner_integration();
    test_override_via_c_abi();

    if (g_failed > 0) {
        std::fprintf(stderr, "game_feature_registry_test: %d/%d checks FAILED\n",
                     g_failed, g_checked);
        return 1;
    }
    std::printf("game_feature_registry_test: all %d checks passed\n", g_checked);
    return 0;
}
