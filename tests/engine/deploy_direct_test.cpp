// Engine test for DirectDeployStrategy (Workspace-uoy.3).
//
// Pins the direct-deploy lifecycle contract:
//   1. Per-file deploy/remove via the inherited DeploymentStrategy interface
//      delegate to SymlinkStrategy (executables copied, data symlinked).
//   2. deploy_all() walks enabled mods and symlinks them into game_dir with a
//      persistent ledger; disabled mods and special dirs are skipped.
//   3. sync() is the O(Δ) path: an unchanged re-run touches zero files, adding
//      a mod deploys only its files, disabling a mod unlinks its stale files.
//   4. undeploy() removes every deployed artifact, restores backed-up
//      originals, and drops the ledger.
//   5. Backup/restore: a game-native file a mod overrides is parked in
//      Original_Files on first collision and restored on undeploy.
//   6. Queries: is_deployed / list_deployed / current_ledger reflect the
//      ledger state.
#include "engine/deploy/strategy_direct.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {
void check(bool cond, const char* what) {
    INFO(what);
    REQUIRE(cond);
}
}

static void write_file(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    out << contents;
    if (!out.good()) {
        std::printf("FAIL: could not write %s\n", p.string().c_str());
        std::exit(1);
    }
}

static std::string read_file(const fs::path& p) {
    std::ifstream in(p);
    std::string s;
    std::getline(in, s);
    return s;
}

// Build a DirectDeployStrategy over a fresh temp instance. The caller owns the
// base dir and removes it at the end.
static engine::DirectDeployStrategy make_strategy(
    const fs::path& base, bool include_mod_id = false) {
    engine::DirectDeployStrategy::Config cfg;
    cfg.mods_dir = base / "mods";
    cfg.game_dir = base / "game";
    cfg.deploy_prefix = "Data";
    cfg.deploy_include_mod_id = include_mod_id;
    cfg.disable_mechanism = ".gmmdisabled";
    cfg.case_sensitive = true;
    cfg.ledger_file = base / ".gmm_deploy_ledger";
    cfg.backup_root = cfg.game_dir / engine::kOriginalFilesDirName;
    fs::create_directories(cfg.game_dir);
    return engine::DirectDeployStrategy(std::move(cfg));
}

TEST_CASE("direct strategy per-file interface", "[engine]") {
    const fs::path base =
        fs::current_path() / ("gmm_test_direct_perfile_" + std::to_string(getpid()));
    fs::create_directories(base);
    auto strategy = make_strategy(base);

    const fs::path mod = base / "mods" / "ToolMod" / "bin";
    write_file(mod / "Tool.exe", "MZ");
    write_file(mod / "data.txt", "x");

    // Inherited DeploymentStrategy::deploy delegates to SymlinkStrategy:
    // executables become real copies, data files stay symlinks.
    const fs::path exe_target = base / "game" / "bin" / "Tool.exe";
    const fs::path data_target = base / "game" / "bin" / "data.txt";
    check(strategy.deploy(mod / "Tool.exe", exe_target),
          "per-file deploy of an executable");
    check(strategy.deploy(mod / "data.txt", data_target),
          "per-file deploy of a data file");
    check(fs::is_regular_file(exe_target) && !fs::is_symlink(exe_target),
          "executable is a real copy, not a symlink");
    check(fs::is_symlink(data_target), "data file stays a symlink");

    // Inherited DeploymentStrategy::remove removes deployed symlinks. Copied
    // executables are regular files, and SymlinkStrategy::remove only removes
    // symlinks (the full cleanup path for copies is undeploy(), which uses
    // remove_deployed_files and handles both kinds).
    check(strategy.remove(data_target), "per-file remove of a symlink");
    check(!fs::exists(data_target), "removed symlink is gone");
    check(!strategy.remove(exe_target),
          "per-file remove leaves copied executables alone (SymlinkStrategy contract)");
    check(fs::is_regular_file(exe_target),
          "copied executable untouched by per-file remove");

    fs::remove_all(base);
}

TEST_CASE("direct strategy deploy_all and queries", "[engine]") {
    const fs::path base =
        fs::current_path() / ("gmm_test_direct_deployall_" + std::to_string(getpid()));
    fs::create_directories(base);
    auto strategy = make_strategy(base);

    // One enabled mod, one disabled mod, and the special dirs deploy skips.
    write_file(base / "mods" / "EnabledMod" / "RaceMenu.esp", "x");
    write_file(base / "mods" / "EnabledMod" / "Meshes" / "a.nif", "x");
    write_file(base / "mods" / "DisabledMod" / "SkyUI.esp", "x");
    write_file(base / "mods" / "DisabledMod" / ".gmmdisabled", "x");
    write_file(base / "mods" / "Overwrite" / "stray.txt", "x");
    write_file(base / "mods" / "MERGED" / "merged.txt", "x");

    std::vector<std::pair<int, int>> progress;
    check(strategy.deploy_all([&progress](int done, int total) {
              progress.emplace_back(done, total);
          }),
          "deploy_all succeeds");

    // Enabled mod deployed as symlinks into game_dir.
    const fs::path deployed = base / "game" / "Data" / "RaceMenu.esp";
    check(fs::is_symlink(deployed), "enabled mod deployed as symlink");
    check(fs::exists(deployed), "deployed file resolves");
    check(fs::read_symlink(deployed) ==
              base / "mods" / "EnabledMod" / "RaceMenu.esp",
          "symlink points back into mods");
    check(fs::is_symlink(base / "game" / "Data" / "Meshes" / "a.nif"),
          "nested file deployed");

    // Disabled mod and special dirs skipped.
    check(!fs::exists(base / "game" / "Data" / "SkyUI.esp"),
          "disabled mod not deployed");
    check(!fs::exists(base / "game" / "Data" / "Overwrite"),
          "Overwrite dir skipped");
    check(!fs::exists(base / "game" / "Data" / "MERGED"),
          "MERGED dir skipped");

    // Progress reported and completes at the total (2 files).
    check(!progress.empty(), "progress reported");
    check(progress.back().second == 2, "progress total is the work count");
    check(progress.back().first == 2, "progress completes at the total");

    // Persistent ledger written at the instance root.
    check(fs::is_regular_file(base / ".gmm_deploy_ledger"),
          "ledger persisted at instance root");

    // Queries reflect the ledger.
    check(strategy.is_deployed(deployed), "is_deployed true for a deployed path");
    check(!strategy.is_deployed(base / "game" / "Data" / "SkyUI.esp"),
          "is_deployed false for a non-deployed path");
    const auto ledger = strategy.current_ledger();
    check(ledger.size() == 2, "current_ledger has both deployed files");
    check(ledger.count(deployed) == 1, "current_ledger contains the target");
    const auto listed = strategy.list_deployed();
    check(listed.size() == 2, "list_deployed has both deployed files");
    bool found = false;
    for (const auto& info : listed) {
        if (info.target == deployed) {
            found = true;
            check(info.source == base / "mods" / "EnabledMod" / "RaceMenu.esp",
                  "list_deployed source matches");
        }
    }
    check(found, "list_deployed contains the expected entry");

    fs::remove_all(base);
}

TEST_CASE("direct strategy sync is O(delta)", "[engine]") {
    const fs::path base =
        fs::current_path() / ("gmm_test_direct_sync_" + std::to_string(getpid()));
    fs::create_directories(base);
    auto strategy = make_strategy(base);

    // Run 1: AMod and CMod fight over Data/shared.esp; lexicographically last
    // (CMod) wins. CMod also owns cunique.esp.
    write_file(base / "mods" / "AMod" / "shared.esp", "A");
    write_file(base / "mods" / "CMod" / "shared.esp", "C");
    write_file(base / "mods" / "CMod" / "cunique.esp", "C");

    auto result = strategy.sync();
    check(result.files_failed == 0, "first sync succeeds");
    check(result.files_deployed == 2, "first sync deploys both files");
    check(result.files_removed == 0, "first sync removes nothing");
    check(result.files_unchanged == 0, "first sync has no unchanged entries");
    const fs::path shared = base / "game" / "Data" / "shared.esp";
    check(fs::read_symlink(shared) == base / "mods" / "CMod" / "shared.esp",
          "last-in-folder-order mod wins the contested file");

    // Run 2: unchanged re-run touches zero files (ledger O(Δ)).
    result = strategy.sync();
    check(result.files_failed == 0, "unchanged sync succeeds");
    check(result.files_deployed == 0, "unchanged sync deploys nothing");
    check(result.files_removed == 0, "unchanged sync removes nothing");
    check(result.files_unchanged == 2, "unchanged sync counts both as unchanged");

    // Run 3: DMod appears with the same file plus a unique file — it is now
    // the last mod, so it wins the contest and owns dunique.esp.
    write_file(base / "mods" / "DMod" / "shared.esp", "D");
    write_file(base / "mods" / "DMod" / "dunique.esp", "D");
    result = strategy.sync();
    check(result.files_failed == 0, "winner-change sync succeeds");
    check(result.files_deployed == 2,
          "winner-change sync deploys the re-pointed and the new file");
    check(result.files_removed == 0, "winner-change sync removes nothing");
    check(result.files_unchanged == 1,
          "winner-change sync leaves the untouched file alone");
    check(fs::read_symlink(shared) == base / "mods" / "DMod" / "shared.esp",
          "winner change re-points the symlink to the new owner");

    // Run 4: DMod disabled -> dunique.esp unlinked, contest reverts to CMod.
    write_file(base / "mods" / "DMod" / ".gmmdisabled", "x");
    result = strategy.sync();
    check(result.files_failed == 0, "disable sync succeeds");
    check(result.files_deployed == 1,
          "disable sync re-points the contested file to the new winner");
    check(result.files_removed == 1,
          "disable sync unlinks the disabled mod's unique file");
    check(!fs::exists(base / "game" / "Data" / "dunique.esp"),
          "disabled mod's file unlinked from game_dir");
    check(fs::read_symlink(shared) == base / "mods" / "CMod" / "shared.esp",
          "contest re-points to the next winner after disable");

    fs::remove_all(base);
}

TEST_CASE("direct strategy undeploy restores originals", "[engine]") {
    const fs::path base =
        fs::current_path() / ("gmm_test_direct_undeploy_" + std::to_string(getpid()));
    fs::create_directories(base);
    auto strategy = make_strategy(base);

    // A game-native file that a mod overrides, plus a native file no mod
    // touches.
    write_file(base / "game" / "Data" / "original.esp", "original");
    write_file(base / "game" / "Data" / "native.esp", "native");
    write_file(base / "mods" / "AMod" / "original.esp", "modded");
    write_file(base / "mods" / "AMod" / "newfile.esp", "new");

    check(strategy.deploy_all(), "first deploy succeeds");
    const fs::path original_target = base / "game" / "Data" / "original.esp";
    check(fs::is_symlink(original_target), "mod wins the collided path");
    const fs::path backup = base / "game" / "Original_Files" / "Data" / "original.esp";
    check(fs::is_regular_file(backup), "original backed up into Original_Files");
    check(read_file(backup) == "original", "backup preserves original content");
    check(strategy.is_deployed(original_target), "collided path is deployed");

    // undeploy: mod artifacts gone, originals restored, ledger dropped.
    check(strategy.undeploy(), "undeploy succeeds");
    check(fs::is_regular_file(original_target) && !fs::is_symlink(original_target),
          "collided path restored as the original file");
    check(read_file(original_target) == "original",
          "undeploy leaves original content");
    check(!fs::exists(base / "game" / "Data" / "newfile.esp"),
          "mod file removed by undeploy");
    check(fs::is_regular_file(base / "game" / "Data" / "native.esp"),
          "untouched game file stays");
    check(!fs::exists(backup), "backup store empty after undeploy");
    check(!fs::exists(base / ".gmm_deploy_ledger"),
          "ledger dropped after full removal");
    check(strategy.current_ledger().empty(), "current_ledger empty after undeploy");
    check(strategy.list_deployed().empty(), "list_deployed empty after undeploy");

    // A fresh deploy re-backs-up the restored original.
    check(strategy.deploy_all(), "deploy after undeploy succeeds");
    check(fs::is_symlink(original_target), "post-undeploy deploy wins again");
    check(fs::is_regular_file(backup),
          "post-undeploy deploy re-backs-up the original");

    fs::remove_all(base);
}

TEST_CASE("direct strategy include-mod-id layout", "[engine]") {
    const fs::path base =
        fs::current_path() / ("gmm_test_direct_bymod_" + std::to_string(getpid()));
    fs::create_directories(base);
    auto strategy = make_strategy(base, /*include_mod_id=*/true);

    write_file(base / "mods" / "AMod" / "shared.esp", "A");
    write_file(base / "mods" / "CMod" / "shared.esp", "C");

    check(strategy.deploy_all(), "include-mod-id deploy succeeds");
    // Each mod gets its own Data/<folder>/ subtree, so contested paths coexist.
    check(fs::is_symlink(base / "game" / "Data" / "AMod" / "shared.esp"),
          "include-mod-id keeps both contesting files");
    check(fs::is_symlink(base / "game" / "Data" / "CMod" / "shared.esp"),
          "include-mod-id keeps both contesting files (2)");

    // list_deployed derives mod_id from the deploy_prefix path structure.
    const auto listed = strategy.list_deployed();
    check(listed.size() == 2, "list_deployed has both files");
    bool saw_a = false;
    bool saw_c = false;
    for (const auto& info : listed) {
        if (info.mod_id == "AMod") saw_a = true;
        if (info.mod_id == "CMod") saw_c = true;
    }
    check(saw_a && saw_c, "mod_id derived from the deployed path");

    fs::remove_all(base);
}