// Engine test for per-folder instance path overrides.
//
// Covers: defaults under <root>/<kind>, override-aware path_for (incl. the
// cache-derived archives/thumbnails), set_path_override clearing back to the
// default, and the instance.toml roundtrip (only non-empty overrides are
// persisted; a read-back instance resolves the same paths).
//
// Also covers Workspace-wk8 (generic instance creation): a game-less
// instance (empty install_path) is creatable and round-trips, and
// deploy_config_for leaves backup_root empty instead of deriving a
// CWD-relative path.
#include "engine/core/instance/instance.h"
#include "engine/core/instance/instance_utils.h"
#include "engine/deploy/deploy_utils.h"
#include "engine/game/detect/game_detector.h"
#include "engine/game/registry/game_knowledge.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {
void require(bool cond, const char* msg) {
    INFO(msg);
    REQUIRE(cond);
}
}

TEST_CASE("instance path", "[engine]") {
    using engine::Instance;
    using engine::InstanceKind;

    const fs::path root = "/tmp/gmm_instance_path/instances/Test";

    // --- Defaults live under the instance root. ---
    Instance inst = Instance::from_root(root);
    require(inst.path_for(InstanceKind::Mods) == root / "mods",
            "default mods dir is <root>/mods");
    require(inst.path_for(InstanceKind::Downloads) == root / "downloads",
            "default downloads dir is <root>/downloads");
    require(inst.path_for(InstanceKind::Cache) == root / "cache",
            "default cache dir is <root>/cache");
    require(inst.path_for(InstanceKind::Profiles) == root / "profiles",
            "default profiles dir is <root>/profiles");
    require(inst.path_for(InstanceKind::Overwrite) == root / "overwrite",
            "default overwrite dir is <root>/overwrite");
    require(inst.path_for(InstanceKind::Meta) == root / "meta",
            "default meta dir is <root>/meta");

    // --- Overrides replace the defaults. ---
    const fs::path mods = "/data/mods";
    const fs::path dl = "/data/archives";
    const fs::path cache = "/data/cache";
    const fs::path profiles = "/data/profiles";
    const fs::path overwrite = "/data/overwrite";
    inst.set_path_override(InstanceKind::Mods, mods);
    inst.set_path_override(InstanceKind::Downloads, dl);
    inst.set_path_override(InstanceKind::Cache, cache);
    inst.set_path_override(InstanceKind::Profiles, profiles);
    inst.set_path_override(InstanceKind::Overwrite, overwrite);
    require(inst.path_for(InstanceKind::Mods) == mods, "mods override wins");
    require(inst.path_for(InstanceKind::Downloads) == dl, "downloads override wins");
    require(inst.path_for(InstanceKind::Cache) == cache, "cache override wins");
    require(inst.path_for(InstanceKind::Profiles) == profiles, "profiles override wins");
    require(inst.path_for(InstanceKind::Overwrite) == overwrite, "overwrite override wins");
    require(inst.path_for(InstanceKind::Meta) == root / "meta",
            "non-overridable kinds keep the default");
    require(inst.path_override(InstanceKind::Mods) == mods, "path_override returns the value");
    require(inst.path_override(InstanceKind::Meta).empty(),
            "path_override is empty for non-overridable kinds");

    // --- Cache-derived folders follow the cache override. ---
    require(inst.path_for(InstanceKind::CacheArchives) == cache / "archives",
            "archives follow the cache override");
    require(inst.path_for(InstanceKind::CacheThumbnails) == cache / "thumbnails",
            "thumbnails follow the cache override");

    // --- Clearing an override falls back to the default. ---
    inst.set_path_override(InstanceKind::Cache, {});
    require(inst.path_for(InstanceKind::Cache) == root / "cache",
            "cleared cache override falls back to the default");
    require(inst.path_for(InstanceKind::CacheArchives) == root / "cache" / "archives",
            "cleared cache override resets archives default");
    require(inst.path_override(InstanceKind::Cache).empty(), "cleared override reads empty");
    inst.set_path_override(InstanceKind::Cache, cache);

    // --- instance.toml roundtrip. ---
    fs::remove_all(root);
    fs::create_directories(root);
    require(inst.write_toml(), "write_toml succeeds");

    Instance read_back = Instance::from_root(root);
    require(read_back.read_toml(), "read_toml succeeds");
    require(read_back.path_for(InstanceKind::Mods) == mods, "mods override survives toml");
    require(read_back.path_for(InstanceKind::Downloads) == dl, "downloads override survives toml");
    require(read_back.path_for(InstanceKind::Cache) == cache, "cache override survives toml");
    require(read_back.path_for(InstanceKind::Profiles) == profiles, "profiles override survives toml");
    require(read_back.path_for(InstanceKind::Overwrite) == overwrite, "overwrite override survives toml");
    require(read_back.path_for(InstanceKind::Meta) == root / "meta",
            "non-overridden kinds stay default after roundtrip");

    // Only non-empty overrides are written.
    Instance partial = Instance::from_root(root);
    partial.info().game_id = "test_game";
    partial.set_path_override(InstanceKind::Mods, mods);
    require(partial.write_toml(), "partial write_toml succeeds");
    Instance partial_back = Instance::from_root(root);
    require(partial_back.read_toml(), "partial read_toml succeeds");
    require(partial_back.path_for(InstanceKind::Mods) == mods, "mods override roundtrips");
    require(partial_back.path_for(InstanceKind::Downloads) == root / "downloads",
            "unset override stays default after write");
    require(partial_back.info().game_id == "test_game",
            "existing keys preserved alongside new override keys");

    // --- write_key surgical roundtrip (proton_runner). ---
    Instance runner = Instance::from_root(root);
    require(runner.write_key("proton_runner", "Proton 10.0"),
            "write_key sets proton_runner");
    Instance runner_back = Instance::from_root(root);
    require(runner_back.read_toml(), "read_toml after write_key succeeds");
    require(runner_back.info().proton_runner == "Proton 10.0",
            "proton_runner roundtrips through toml");
    require(runner_back.path_for(InstanceKind::Mods) == mods,
            "write_key preserves existing override sections");
    require(runner_back.info().game_id == "test_game",
            "write_key preserves unrelated top-level keys");

    // write_key with an absolute path survives too.
    require(runner.write_key("proton_runner", "/opt/proton/proton"),
            "write_key accepts absolute paths");
    Instance abs_back = Instance::from_root(root);
    require(abs_back.read_toml(), "read_toml after absolute write_key succeeds");
    require(abs_back.info().proton_runner == "/opt/proton/proton",
            "absolute proton_runner roundtrips");

    // Empty value removes the key.
    require(runner.write_key("proton_runner", ""), "write_key with empty value succeeds");
    Instance cleared_back = Instance::from_root(root);
    require(cleared_back.read_toml(), "read_toml after clearing succeeds");
    require(cleared_back.info().proton_runner.empty(),
            "cleared proton_runner reads empty");

    // --- deploy_strategy: write_toml + write_key roundtrips. ---
    Instance strat = Instance::from_root(root);
    strat.info().deploy_strategy = "overlayfs";
    require(strat.write_toml(), "write_toml with deploy_strategy succeeds");
    Instance strat_back = Instance::from_root(root);
    require(strat_back.read_toml(), "read_toml after deploy_strategy write succeeds");
    require(strat_back.info().deploy_strategy == "overlayfs",
            "deploy_strategy roundtrips through write_toml");

    Instance strat_key = Instance::from_root(root);
    require(strat_key.write_key("deploy_strategy", "symlink"),
            "write_key sets deploy_strategy");
    Instance strat_key_back = Instance::from_root(root);
    require(strat_key_back.read_toml(), "read_toml after deploy_strategy write_key succeeds");
    require(strat_key_back.info().deploy_strategy == "symlink",
            "deploy_strategy roundtrips through write_key");
    require(strat_key.write_key("deploy_strategy", ""),
            "write_key with empty value clears deploy_strategy");
    Instance strat_cleared = Instance::from_root(root);
    require(strat_cleared.read_toml(), "read_toml after clearing deploy_strategy succeeds");
    require(strat_cleared.info().deploy_strategy.empty(),
            "cleared deploy_strategy reads empty");

    // --- last_tab roundtrip (Issue #21). ---
    Instance tabbed = Instance::from_root(root);
    tabbed.info().game_id = "test_game";
    tabbed.info().last_tab = "plugins";
    require(tabbed.write_toml(), "write_toml with last_tab succeeds");
    Instance tabbed_back = Instance::from_root(root);
    require(tabbed_back.read_toml(), "read_toml after last_tab write succeeds");
    require(tabbed_back.info().last_tab == "plugins",
            "last_tab roundtrips through toml");
    require(tabbed_back.info().game_id == "test_game",
            "last_tab write preserves unrelated top-level keys");

    // write_key surgical roundtrip for last_tab.
    require(tabbed.write_key("last_tab", "downloads"),
            "write_key sets last_tab");
    Instance tab_key_back = Instance::from_root(root);
    require(tab_key_back.read_toml(), "read_toml after last_tab write_key succeeds");
    require(tab_key_back.info().last_tab == "downloads",
            "last_tab write_key roundtrips through toml");
    require(tab_key_back.info().game_id == "test_game",
            "last_tab write_key preserves unrelated top-level keys");

    // Empty last_tab is not persisted; a fresh instance reads empty (defaults
    // to the first tab).
    require(tabbed.write_key("last_tab", ""), "write_key clears last_tab");
    Instance tab_cleared = Instance::from_root(root);
    require(tab_cleared.read_toml(), "read_toml after clearing last_tab succeeds");
    require(tab_cleared.info().last_tab.empty(),
            "cleared last_tab reads empty");
}

// Workspace-4fu: user-chosen instance names. The display name is sanitized
// with spaces preserved (it becomes the folder name), an unsanitizable/empty
// name fails, and creating over an existing instance.toml is refused instead
// of clobbering it.
TEST_CASE("create_instance_for_game custom display name", "[engine]") {
    using engine::Instance;

    const fs::path instances_root = "/tmp/gmm_instance_path/wk4fu_instances";
    fs::remove_all(instances_root);

    engine::DetectedGame game;
    game.game_id = "testgame";
    game.name = "Test Game";

    // Spaces survive sanitization; the folder IS the (sanitized) name.
    Instance inst = engine::create_instance_for_game(
        game, instances_root, "My Skyrim Setup");
    require(inst.info().root.filename() == "My Skyrim Setup",
            "custom name becomes the folder name, spaces preserved");
    require(fs::is_regular_file(inst.info().root / "instance.toml"),
            "instance.toml written for the custom-named instance");

    // Filesystem-unsafe characters are sanitized away.
    Instance unsafe = engine::create_instance_for_game(
        game, instances_root, "a/b:c?d");
    require(unsafe.info().root.filename() == "a_b_c_d",
            "unsafe chars replaced during sanitization");

    // Uniqueness: creating again with the same name must fail, leaving the
    // original instance.toml untouched.
    const std::string toml_before = [&] {
        std::ifstream f(inst.info().root / "instance.toml");
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }();
    Instance dup = engine::create_instance_for_game(
        game, instances_root, "My Skyrim Setup");
    require(dup.info().game_id.empty(),
            "duplicate instance name refused");
    std::ifstream after(inst.info().root / "instance.toml");
    std::ostringstream ss_after;
    ss_after << after.rdbuf();
    require(ss_after.str() == toml_before,
            "existing instance.toml not clobbered");

    // Empty/unusable names fail cleanly.
    Instance blank = engine::create_instance_for_game(game, instances_root, "");
    require(blank.info().game_id.empty(), "empty custom name refused");
    Instance dots = engine::create_instance_for_game(game, instances_root, "..");
    require(dots.info().game_id.empty(), "dot-only name refused");

    fs::remove_all(instances_root);
}

// Workspace-wk8: instance creation must not require a game path. A
// DetectedGame with an empty install_path creates a working game-less
// instance: directories + instance.toml exist, and the toml round-trips
// without a game_dir key (empty = omitted).
TEST_CASE("create_instance_for_game without a game path", "[engine]") {
    using engine::Instance;

    const fs::path instances_root = "/tmp/gmm_instance_path/wk8_instances";
    fs::remove_all(instances_root);

    engine::DetectedGame game;
    game.game_id = "testgame";
    game.name = "Test Game";
    // install_path deliberately empty - the whole point.

    Instance inst = engine::create_instance_for_game(game, instances_root);
    REQUIRE(!inst.info().root.empty());
    REQUIRE(fs::is_directory(inst.info().root / "mods"));
    REQUIRE(fs::is_regular_file(inst.info().root / "instance.toml"));

    // Round-trip: no game_dir key on disk, reads back empty.
    std::string toml;
    {
        std::ifstream f(inst.info().root / "instance.toml");
        REQUIRE(f.is_open());
        std::ostringstream ss;
        ss << f.rdbuf();
        toml = ss.str();
    }
    INFO("instance.toml: " << toml);
    REQUIRE(toml.find("game_dir") == std::string::npos);

    Instance read_back = Instance::from_root(inst.info().root);
    REQUIRE(read_back.read_toml());
    REQUIRE(read_back.info().game_id == "testgame");
    REQUIRE(read_back.info().game_dir.empty());

    fs::remove_all(instances_root);
}

// Workspace-wk8: with an empty game_dir, backup_root must stay empty (the
// documented "caller opts out" state) instead of becoming the CWD-relative
// "Original_Files".
TEST_CASE("deploy_config_for without a game path", "[engine]") {
    engine::GameKnowledge knowledge;
    const fs::path instance_root = "/tmp/gmm_instance_path/wk8_deploy_inst";

    const engine::DeployConfig cfg =
        engine::deploy_config_for(instance_root, {}, knowledge, "testgame");
    REQUIRE(cfg.game_dir.empty());
    REQUIRE(cfg.backup_root.empty());
    REQUIRE(!cfg.mods_dir.empty());
    REQUIRE(!cfg.ledger_file.empty());

    // A real game dir still yields <game_dir>/Original_Files.
    const engine::DeployConfig with_dir = engine::deploy_config_for(
        instance_root, "/games/test", knowledge, "testgame");
    REQUIRE(with_dir.backup_root ==
            fs::path("/games/test") / engine::kOriginalFilesDirName);
}

// Workspace-6up: the "game_mods_dir" deploy-target override. Round-trips
// through instance.toml, and deploy_config_for resolves it into
// DeployConfig.game_mods_dir so every deploy consumer's deploy_target() is
// the actual mods folder while game_dir/backup_root keep their raw meaning.
TEST_CASE("game mods dir override", "[engine]") {
    using engine::Instance;
    using engine::deploy_config_for;

    const fs::path instances_root = "/tmp/gmm_instance_path/wk6up_instances";
    fs::remove_all(instances_root);

    engine::DetectedGame game;
    game.game_id = "testgame";
    game.name = "Test Game";
    Instance inst = engine::create_instance_for_game(game, instances_root);
    REQUIRE(!inst.info().root.empty());

    engine::GameKnowledge knowledge;
    knowledge.set("testgame", "mods_subpath", "Data");

    // --- No override: deploy target falls back to game_dir. ---
    {
        const auto cfg = deploy_config_for(inst.info().root, "/games/test",
                                           knowledge, "testgame");
        REQUIRE(cfg.game_mods_dir.empty());
        REQUIRE(cfg.deploy_target() == fs::path("/games/test"));
        // The plugin-declared data subdir still rides on deploy_prefix.
        REQUIRE(cfg.deploy_prefix == "Data");
        REQUIRE(cfg.backup_root ==
                fs::path("/games/test") / engine::kOriginalFilesDirName);
    }

    // --- Override set: it becomes the deploy target; game_dir-derived
    // fields stay raw. ---
    const fs::path override_dir =
        "/Users/test/Library/Application Support/Binding of Isaac Afterbirth+ Mods";
    REQUIRE(inst.write_key("game_mods_dir", override_dir.string()));
    {
        Instance back = Instance::from_root(inst.info().root);
        REQUIRE(back.read_toml());
        REQUIRE(back.info().game_mods_dir == override_dir);

        const auto cfg = deploy_config_for(inst.info().root, "/games/test",
                                           knowledge, "testgame");
        REQUIRE(cfg.game_mods_dir == override_dir);
        REQUIRE(cfg.deploy_target() == override_dir);
        // Override IS the mods folder: deploy_prefix must be cleared or
        // target_base = deploy_target() / deploy_prefix double-nests
        // (override/"Data") instead of landing files directly.
        REQUIRE(cfg.deploy_prefix.empty());
        REQUIRE(cfg.game_dir == fs::path("/games/test"));
        // Backups still park next to the game install, not inside the
        // external mods folder.
        REQUIRE(cfg.backup_root ==
                fs::path("/games/test") / engine::kOriginalFilesDirName);
    }

    // --- Self-referential guard: override == instance mods dir is dropped
    // (deploying the mods dir into itself). ---
    REQUIRE(inst.write_key("game_mods_dir",
                           (inst.info().root / "mods").string()));
    {
        const auto cfg = deploy_config_for(inst.info().root, "/games/test",
                                           knowledge, "testgame");
        REQUIRE(cfg.game_mods_dir.empty());
        REQUIRE(cfg.deploy_target() == fs::path("/games/test"));
    }

    // --- Clearing the key restores the fallback. ---
    REQUIRE(inst.write_key("game_mods_dir", ""));
    {
        Instance back = Instance::from_root(inst.info().root);
        REQUIRE(back.read_toml());
        REQUIRE(back.info().game_mods_dir.empty());
    }

    fs::remove_all(instances_root);
}

// Workspace-otx: game-specific absolute mods dirs come from the plugin's
// "game_mods_dir" knowledge hook (Isaac on macOS), not from hardcoded engine
// checks. resolve_game_mods_dir is the single resolution chain for scan/UI
// consumers; deploy consumes only the override/plugin steps.
//
// Workspace-s3hn: the chain stops being a "fallback to game_dir or
// game_dir/mods_subpath" once mods_subpath is a DEPLOY target. mods_subpath
// is intentionally NOT a scan source - falling back to it (or to game_dir
// itself when both are empty) would walk vanilla game content (Data/,
// SKSE, Scripts, Meshes, Source, ...) and synthesize it as ScannedMod rows,
// contrary to MO2 (which only reads mods from <profile>/mods/). When a game
// declares no "game_mods_dir" hook, no instance.toml override, and no
// "mod_scan_subpath", the resolution returns an empty path: there is no
// game-dir scan source, full stop.
TEST_CASE("plugin declared game mods dir", "[engine]") {
    using engine::GameKnowledge;
    using engine::plugin_game_mods_dir;
    using engine::resolve_game_mods_dir;

    // --- Undeclared: empty accessor; chain returns empty path (no game-dir
    //     scan source - the scanner falls back to the instance mods dir).
    //     mods_subpath is a deploy target and is not consulted here.
    GameKnowledge plain;
    plain.set("testgame", "mods_subpath", "Data");
    REQUIRE(plugin_game_mods_dir(plain, "testgame").empty());
    REQUIRE(resolve_game_mods_dir("testgame", "/games/test", plain).empty());
    REQUIRE(resolve_game_mods_dir("othergame", "/games/test", plain).empty());

    // --- Plugin-declared ~ path expands against $HOME. ---
    GameKnowledge isaac;
    isaac.set("TheBindingOfIsaacRebirth", "game_mods_dir",
              "~/Library/Application Support/Binding of Isaac Afterbirth+ Mods");
    const auto expanded =
        fs::path(std::getenv("HOME") ? std::getenv("HOME") : "") /
        "Library/Application Support/Binding of Isaac Afterbirth+ Mods";
    REQUIRE(plugin_game_mods_dir(isaac, "TheBindingOfIsaacRebirth") ==
            expanded.string());
    REQUIRE(resolve_game_mods_dir("TheBindingOfIsaacRebirth", "/games/isaac",
                                  isaac) == expanded);
    // Other games are unaffected by the declaration: still no game-dir scan
    // source (no "game_mods_dir" or "mod_scan_subpath" for testgame).
    REQUIRE(resolve_game_mods_dir("testgame", "/games/test", isaac).empty());

    // --- Instance override beats the plugin declaration. ---
    REQUIRE(resolve_game_mods_dir("TheBindingOfIsaacRebirth", "/games/isaac",
                                  isaac, "/custom/mods") ==
            fs::path("/custom/mods"));

    // --- Deploy honors the plugin key only when the user did not override,
    // and never folds mods_subpath into the deploy root. ---
    const fs::path instances_root = "/tmp/gmm_instance_path/wkotx_instances";
    fs::remove_all(instances_root);
    engine::DetectedGame game;
    game.game_id = "testgame";
    game.name = "Test Game";
    engine::Instance inst = engine::create_instance_for_game(game, instances_root);
    REQUIRE(!inst.info().root.empty());

    {
        const auto cfg = engine::deploy_config_for(
            inst.info().root, "/games/test", isaac, "TheBindingOfIsaacRebirth");
        REQUIRE(cfg.game_mods_dir == expanded);
        REQUIRE(cfg.deploy_target() == expanded);
    }
    {
        // mods_subpath-only games keep the classic layout (prefix carries it).
        const auto cfg = engine::deploy_config_for(
            inst.info().root, "/games/test", plain, "testgame");
        REQUIRE(cfg.game_mods_dir.empty());
        REQUIRE(cfg.deploy_target() == fs::path("/games/test"));
        REQUIRE(cfg.deploy_prefix == "Data");
    }
    // User override in instance.toml still wins over the plugin key.
    REQUIRE(inst.write_key("game_mods_dir", "/custom/mods"));
    {
        const auto cfg = engine::deploy_config_for(
            inst.info().root, "/games/test", isaac, "TheBindingOfIsaacRebirth");
        REQUIRE(cfg.game_mods_dir == fs::path("/custom/mods"));
    }

    fs::remove_all(instances_root);
}

// Workspace-l6w: instance names are user-chosen and may contain spaces.
// to_instance_name keeps spaces, strips filesystem-unsafe chars (including
// control chars/NUL), trims dots/whitespace at both ends so ".", ".." and
// "..." degrade to "".
TEST_CASE("to_instance_name sanitization", "[engine]") {
    using engine::Instance;

    // Spaces are filesystem-safe and preserved.
    REQUIRE(Instance::to_instance_name("My Skyrim Setup") == "My Skyrim Setup");
    // Invalid chars stripped, inner content kept.
    REQUIRE(Instance::to_instance_name(R"(a/b\c:d*e?f"g<h>i|j)") ==
            "abcdefghij");
    // Control characters (incl. NUL) stripped.
    REQUIRE(Instance::to_instance_name(std::string("a\x01" "b\x7f" "c")) == "abc");
    REQUIRE(Instance::to_instance_name(std::string("a\0b", 3)) == "ab");
    // Dot-only and degenerate names sanitize to empty.
    REQUIRE(Instance::to_instance_name(".") == "");
    REQUIRE(Instance::to_instance_name("..") == "");
    REQUIRE(Instance::to_instance_name("...") == "");
    REQUIRE(Instance::to_instance_name("") == "");
    REQUIRE(Instance::to_instance_name("   ") == "");
    // Leading/trailing dots and whitespace trimmed; inner dots survive.
    REQUIRE(Instance::to_instance_name(" .name. ") == "name");
    REQUIRE(Instance::to_instance_name("The.Dot.Game") == "The.Dot.Game");
}

// Workspace-l6w: creation-time uniqueness. Names disambiguate with " 2",
// " 3", ... against anything existing under the root (dirs AND files), and
// degenerate names fall back instead of producing an empty folder name.
TEST_CASE("unique_instance_name", "[engine]") {
    namespace fs = std::filesystem;
    using engine::unique_instance_name;

    const fs::path root = "/tmp/gmm_instance_path/l6w_unique";
    fs::remove_all(root);
    fs::create_directories(root);

    REQUIRE(unique_instance_name("My Setup", root) == "My Setup");
    fs::create_directory(root / "My Setup");
    REQUIRE(unique_instance_name("My Setup", root) == "My Setup 2");
    fs::create_directory(root / "My Setup 2");
    REQUIRE(unique_instance_name("My Setup", root) == "My Setup 3");

    // Plain files block a name too (an instance dir can't be created there).
    { std::ofstream f(root / "Other"); f << "x"; }
    REQUIRE(unique_instance_name("Other", root) == "Other 2");

    // Degenerate input falls back to a usable name.
    REQUIRE(unique_instance_name("...", root) == "New Instance");

    fs::remove_all(root);
}

// Workspace-l6w: create_instance_for_game derives a unique folder per call
// and persists the raw display name as instance.toml "name"; the display
// helper falls back to the folder basename for legacy instances.
TEST_CASE("create_instance_for_game unique names and display name",
          "[engine]") {
    using engine::Instance;

    const fs::path instances_root = "/tmp/gmm_instance_path/l6w_create";
    fs::remove_all(instances_root);

    engine::DetectedGame game;
    game.game_id = "testgame";
    game.name = "Test Game";

    Instance first = engine::create_instance_for_game(game, instances_root);
    REQUIRE(first.info().root.filename() == "Test Game");
    REQUIRE(first.info().display_name == "Test Game");

    Instance second = engine::create_instance_for_game(game, instances_root);
    REQUIRE(second.info().root.filename() == "Test Game 2");
    REQUIRE(fs::is_regular_file(second.info().root / "instance.toml"));

    // Display name round-trips through instance.toml.
    REQUIRE(engine::instance_display_name(second.info().root) == "Test Game");

    // Legacy fallback: no "name" key -> folder basename.
    REQUIRE(first.write_key("name", ""));
    REQUIRE(engine::instance_display_name(first.info().root) ==
            first.info().root.filename().string());

    fs::remove_all(instances_root);
}
