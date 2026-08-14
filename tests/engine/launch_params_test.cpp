// Engine regression test pinning the launch-deploy contract.
//
// GUI and CLI game launches MUST go through engine::prepare_launch_params. It
// honors the per-game "deploy_strategy" knowledge key:
//   - overlayfs games deploy all enabled mods into <instance>/.gmm_staging and
//     add that dir to LaunchParams::extra_lowerdirs on EVERY launch (a previous
//     GUI-only regression skipped the deploy and merely reused whatever was
//     already in .gmm_staging, which the watchdog wipes at every session end,
//     so launches after the first game session saw no mods at all), and
//   - symlink games (the default) deploy straight into game_dir against a
//     persistent ledger at <instance>/.gmm_deploy_ledger.
// This test pins the contract: deploy happens, disabled mods and special dirs
// are skipped, the deploy is idempotent, staging is the overlay lowerdir
// (overlay mode), and direct mode writes into game_dir with no staging or
// lowerdirs.
#include "engine/core/instance/instance_utils.h"
#include "engine/core/util/fs_utils.h"
#include "engine/deploy/launch/launcher.h"
#include "engine/deploy/launch/overlay_launcher.h"
#include "engine/game/registry/game_knowledge.h"

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
void require(bool cond, const std::string& msg) {
    INFO(msg);
    REQUIRE(cond);
}
}

static void write_file(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    out << contents;
    require(out.good(), "write_file failed for " + p.string());
}

// Fake instance: one enabled mod, one disabled mod (disable_mechanism
// marker), and the special dirs deploy must skip.
static void make_instance(const fs::path& root) {
    fs::create_directories(root / "mods" / "EnabledMod");
    write_file(root / "mods" / "EnabledMod" / "RaceMenu.esp", "x");
    fs::create_directories(root / "mods" / "DisabledMod");
    write_file(root / "mods" / "DisabledMod" / "SkyUI.esp", "x");
    write_file(root / "mods" / "DisabledMod" / "disabled.txt", "x");
    fs::create_directories(root / "mods" / "Overwrite");
    write_file(root / "mods" / "Overwrite" / "stray.txt", "x");
    fs::create_directories(root / "mods" / "MERGED");
    write_file(root / "mods" / "MERGED" / "merged.txt", "x");
}

static engine::GameKnowledge make_knowledge(bool include_mod_id,
                                            const std::string& strategy = "overlayfs") {
    engine::GameKnowledge k;
    k.set("testgame", "deploy_prefix", "Data");
    k.set("testgame", "deploy_include_mod_id", include_mod_id ? "true" : "false");
    k.set("testgame", "disable_mechanism", "disabled.txt");
    k.set("testgame", "deploy_strategy", strategy);
    return k;
}

static void check_deploy(const fs::path& root, bool include_mod_id,
                         const std::string& label) {
    make_instance(root);
    const fs::path game_dir = root / "game";
    fs::create_directories(game_dir);
    const fs::path exe = game_dir / "Game.exe";
    write_file(exe, "bin");

    const auto params = engine::prepare_launch_params(
        root, game_dir, exe, make_knowledge(include_mod_id),
        "testgame", 12345, true);

    // Field passthrough.
    require(params.executable == exe, label + ": executable passthrough");
    require(params.game_dir == game_dir, label + ": game_dir passthrough");
    require(params.steam_appid == 12345, label + ": steam_appid passthrough");
    require(params.is_windows_exe, label + ": is_windows_exe passthrough");
    require(params.overwrite_dir == root / "overwrite",
            label + ": overwrite_dir is <root>/overwrite");

    // Staging dir exists and is the sole extra lowerdir.
    require(params.extra_lowerdirs.size() == 1,
            label + ": exactly one extra lowerdir");
    require(params.extra_lowerdirs.front() == root / ".gmm_staging",
            label + ": lowerdir is <root>/.gmm_staging");
    require(fs::is_directory(root / ".gmm_staging"),
            label + ": .gmm_staging created");

    // Enabled mod file deployed as a symlink back into the mods folder.
    const fs::path deployed = include_mod_id
        ? root / ".gmm_staging" / "Data" / "EnabledMod" / "RaceMenu.esp"
        : root / ".gmm_staging" / "Data" / "RaceMenu.esp";
    require(fs::is_symlink(deployed), label + ": enabled mod deployed as symlink");
    require(fs::exists(deployed), label + ": deployed file resolves");
    require(fs::read_symlink(deployed) == root / "mods" / "EnabledMod" / "RaceMenu.esp",
            label + ": symlink points back into mods");

    // Disabled mod NOT deployed.
    const fs::path disabled = include_mod_id
        ? root / ".gmm_staging" / "Data" / "DisabledMod" / "SkyUI.esp"
        : root / ".gmm_staging" / "Data" / "SkyUI.esp";
    require(!fs::exists(disabled), label + ": disabled mod not deployed");

    // Special dirs skipped.
    require(!fs::exists(root / ".gmm_staging" / "Data" / "Overwrite"),
            label + ": Overwrite dir skipped");
    require(!fs::exists(root / ".gmm_staging" / "Data" / "MERGED"),
            label + ": MERGED dir skipped");

    // Idempotent redeploy: a second launch must not fail (EEXIST) and must
    // leave the deployed symlink intact.
    const auto params2 = engine::prepare_launch_params(
        root, game_dir, exe, make_knowledge(include_mod_id),
        "testgame", 12345, true);
    require(params2.extra_lowerdirs.size() == 1,
            label + ": redeploy still yields one lowerdir");
    require(fs::is_symlink(deployed), label + ": redeploy keeps the symlink");
    require(fs::exists(deployed), label + ": redeploy keeps the file");
}

// Request-based overload (P8.4): the engine call is synchronous and returns
// ONLY once the staging tree is fully populated, and it reports link-operation
// progress through the callback (monotonic, completing at the total). The
// GUI's async launch chains on that completion, so this return-contract is
// what makes the launch-never-before-deploy ordering provable.
static void check_deploy_request(const fs::path& root, const std::string& label) {
    make_instance(root);
    const fs::path game_dir = root / "game";
    fs::create_directories(game_dir);
    const fs::path exe = game_dir / "Game.exe";
    write_file(exe, "bin");

    engine::LaunchPrepRequest req;
    req.instance_root = root;
    req.game_dir = game_dir;
    req.executable = exe;
    req.knowledge = make_knowledge(false);
    req.game_id = "testgame";
    req.steam_appid = 12345;
    req.is_windows_exe = true;
    req.environment = {"WINEDEBUG=+file", "GMM_TEST_FLAG=1"};
    req.args = {"-foo", "bar baz"};
    req.cwd = game_dir / "bin";

    std::vector<std::pair<int, int>> progress;
    const auto params = engine::prepare_launch_params(
        req, [&progress](int done, int total) {
            progress.emplace_back(done, total);
        });

    require(params.extra_lowerdirs.size() == 1,
            label + ": request overload yields one lowerdir");
    const fs::path deployed = root / ".gmm_staging" / "Data" / "RaceMenu.esp";
    require(fs::is_symlink(deployed), label + ": deploy finished before return");
    require(params.environment == req.environment,
            label + ": environment overrides forwarded to LaunchParams");
    require(params.environment.size() == 2 &&
            params.environment[0] == "WINEDEBUG=+file",
            label + ": environment order preserved");
    require(params.args == req.args,
            label + ": args forwarded to LaunchParams");
    require(params.args.size() == 2 && params.args[1] == "bar baz",
            label + ": args order and quoting preserved");
    require(params.cwd == req.cwd,
            label + ": cwd forwarded to LaunchParams");

    // Progress contract: monotonic and completed exactly at the total, which
    // is non-zero on the first-ever deploy.
    bool monotonic = true;
    int last = 0;
    int total = 0;
    for (const auto& [d, t] : progress) {
        if (d < last || d > t) monotonic = false;
        last = d;
        total = t;
    }
    require(!progress.empty(), label + ": progress reported");
    require(total > 0, label + ": first deploy reports non-zero total");
    require(monotonic, label + ": progress monotonic");
    require(last == total, label + ": progress completes at the total");
    require(fs::is_symlink(deployed),
            label + ": staging complete when prepare_launch_params returned");
}

// Case-insensitive game (knowledge case_sensitive=false): the launch deploy
// must merge CI-equal directory paths (Meshes/ vs meshes/) into one canonical
// casing in the staging tree, or the case-sensitive overlay mount splits a mod
// across two dirs (XP32 ships both spellings with different files).
static void check_ci_deploy(const fs::path& root, const std::string& label) {
    fs::create_directories(root / "mods" / "CI_Mod" / "Meshes");
    write_file(root / "mods" / "CI_Mod" / "Meshes" / "a.nif", "x");
    fs::create_directories(root / "mods" / "CI_Mod" / "meshes");
    write_file(root / "mods" / "CI_Mod" / "meshes" / "b.nif", "x");

    const fs::path game_dir = root / "game";
    fs::create_directories(game_dir);
    const fs::path exe = game_dir / "Game.exe";
    write_file(exe, "bin");

    engine::GameKnowledge k;
    k.set("testgame", "deploy_prefix", "Data");
    k.set("testgame", "deploy_include_mod_id", "false");
    k.set("testgame", "case_sensitive", "false");
    k.set("testgame", "deploy_strategy", "overlayfs");

    const auto params = engine::prepare_launch_params(
        root, game_dir, exe, k, "testgame", 12345, true);

    const fs::path data = root / ".gmm_staging" / "Data";
    // Exactly one REAL directory among the CI-equal spellings; the other may
    // exist only as the deploy's lowercase alias symlink (game resolves the
    // lower spelling through it). fs::exists follows symlinks, so realness is
    // checked via symlink_status.
    const bool real_upper =
        fs::is_directory(fs::symlink_status(data / "Meshes"));
    const bool real_lower =
        fs::is_directory(fs::symlink_status(data / "meshes"));
    require(real_upper != real_lower,
            label + ": CI-equal dirs merge into exactly one real casing");
    const fs::path merged = real_upper ? data / "Meshes" : data / "meshes";
    // The non-canonical spelling, if present, is a lowercase alias symlink.
    const fs::path alias = real_upper ? data / "meshes" : data / "Meshes";
    require(!fs::exists(alias) || fs::is_symlink(alias),
            label + ": non-canonical spelling is only a lowercase alias");
    require(fs::exists(merged / "a.nif"), label + ": upper-spelling file deployed");
    require(fs::exists(merged / "b.nif"), label + ": lower-spelling file deployed");
    require(fs::is_symlink(merged / "a.nif"), label + ": merged files are symlinks");
    require(fs::exists(params.extra_lowerdirs.front()),
            label + ": staging still the overlay lowerdir");
}

// Direct-symlink mode (deploy_strategy=symlink, the default): mods deploy
// straight into game_dir - no staging dir, no extra_lowerdirs, use_overlay is
// false - and a persistent ledger at <instance>/.gmm_deploy_ledger records
// what was deployed so owner changes across sessions are detected. Executables
// are copied (real files); everything else is a symlink back into mods.
static void check_deploy_direct(const fs::path& root, bool include_mod_id,
                                const std::string& label) {
    make_instance(root);
    const fs::path game_dir = root / "game";
    fs::create_directories(game_dir);
    const fs::path exe = game_dir / "Game.exe";
    write_file(exe, "bin");

    const auto params = engine::prepare_launch_params(
        root, game_dir, exe, make_knowledge(include_mod_id, "symlink"),
        "testgame", 12345, true);

    // Direct mode: no overlay, no staging, no lowerdirs.
    require(!params.use_overlay, label + ": direct mode use_overlay=false");
    require(params.extra_lowerdirs.empty(),
            label + ": direct mode has no extra lowerdirs");
    require(!fs::exists(root / ".gmm_staging"),
            label + ": direct mode never creates .gmm_staging");

    // Enabled mod deployed as a symlink INTO game_dir.
    const fs::path deployed = include_mod_id
        ? game_dir / "Data" / "EnabledMod" / "RaceMenu.esp"
        : game_dir / "Data" / "RaceMenu.esp";
    require(fs::is_symlink(deployed), label + ": enabled mod deployed into game_dir");
    require(fs::exists(deployed), label + ": deployed file resolves");
    require(fs::read_symlink(deployed) == root / "mods" / "EnabledMod" / "RaceMenu.esp",
            label + ": direct symlink points back into mods");

    // Disabled mod NOT deployed.
    const fs::path disabled = include_mod_id
        ? game_dir / "Data" / "DisabledMod" / "SkyUI.esp"
        : game_dir / "Data" / "SkyUI.esp";
    require(!fs::exists(disabled), label + ": disabled mod not deployed in direct mode");

    // Persisted ledger at the instance root (NOT inside the session-wiped
    // staging), so owner changes across sessions are detected.
    require(fs::is_regular_file(root / ".gmm_deploy_ledger"),
            label + ": persistent ledger written at instance root");
    require(!fs::exists(root / ".gmm_staging" / ".gmm_deploy_ledger"),
            label + ": no in-staging ledger in direct mode");

    // Idempotent redeploy: symlink preserved, not EEXIST-failed.
    const auto params2 = engine::prepare_launch_params(
        root, game_dir, exe, make_knowledge(include_mod_id, "symlink"),
        "testgame", 12345, true);
    require(!params2.use_overlay, label + ": redeploy stays direct");
    require(fs::is_symlink(deployed), label + ": redeploy keeps the direct symlink");
    require(fs::exists(deployed), label + ": redeploy keeps the direct file");

    // Game-native file untouched by the deploy.
    require(fs::is_regular_file(exe) && !fs::is_symlink(exe),
            label + ": game-native exe untouched");
}

// Direct-mode O(Δ) redeploy core (deploy_all_enabled_mods_direct): only files
// whose conflict-resolution owner changed are touched. When a new mod wins a
// contested target, the old winner's symlink is re-pointed to the new owner;
// when a mod stops being a winner (disabled), its files are unlinked from
// game_dir.
static void check_direct_winner_change(const fs::path& root,
                                       const std::string& label) {
    const fs::path mods = root / "mods";
    const fs::path game_dir = root / "game";
    fs::create_directories(game_dir);
    const fs::path ledger = root / ".gmm_deploy_ledger";

    engine::GameKnowledge k;
    k.set("testgame", "deploy_prefix", "Data");
    k.set("testgame", "deploy_include_mod_id", "false");
    k.set("testgame", "disable_mechanism", "disabled.txt");

    auto deploy = [&]() {
        return engine::deploy_all_enabled_mods_direct(
            mods, game_dir, "Data", false, "disabled.txt", true, ledger);
    };

    // Run 1: AMod and CMod fight over Data/shared.esp; lexicographically last
    // (CMod) wins.
    fs::create_directories(mods / "AMod");
    write_file(mods / "AMod" / "shared.esp", "A");
    fs::create_directories(mods / "CMod");
    write_file(mods / "CMod" / "shared.esp", "C");
    require(deploy(), label + ": run 1 deploys");
    const fs::path shared = game_dir / "Data" / "shared.esp";
    require(fs::is_symlink(shared), label + ": contested file deployed as symlink");
    require(fs::read_symlink(shared) == mods / "CMod" / "shared.esp",
            label + ": last-in-folder-order mod wins the contested file");

    // Run 2: DMod (sorts after CMod) appears with the same file plus a unique
    // file - it is now the last mod, so it wins the contest and owns dunique.
    fs::create_directories(mods / "DMod");
    write_file(mods / "DMod" / "shared.esp", "D");
    write_file(mods / "DMod" / "dunique.esp", "D");
    require(deploy(), label + ": run 2 deploys");
    require(fs::read_symlink(shared) == mods / "DMod" / "shared.esp",
            label + ": winner change re-points the symlink to the new owner");
    const fs::path dunique = game_dir / "Data" / "dunique.esp";
    require(fs::is_symlink(dunique), label + ": new file deployed");

    // Run 3: DMod disabled -> dunique.esp must be unlinked from game_dir, and
    // the contest reverts to CMod.
    write_file(mods / "DMod" / "disabled.txt", "x");
    require(deploy(), label + ": run 3 deploys");
    require(!fs::exists(dunique),
            label + ": disabled mod's file unlinked from game_dir");
    require(fs::is_symlink(shared), label + ": contested file still deployed");
    require(fs::read_symlink(shared) == mods / "CMod" / "shared.esp",
            label + ": contest re-points to the next winner after disable");

    // Run 4: DMod re-enabled -> both files return, pointing at DMod again.
    fs::remove(mods / "DMod" / "disabled.txt");
    require(deploy(), label + ": run 4 deploys");
    require(fs::is_symlink(shared), label + ": re-enabled file redeployed");
    require(fs::read_symlink(shared) == mods / "DMod" / "shared.esp",
            label + ": re-enabled file points at DMod");
    require(fs::is_symlink(dunique), label + ": re-enabled unique file redeployed");
}

// Direct mode copies executables instead of symlinking them: scripts and
// binaries resolve siblings relative to their own location, so a symlink would
// resolve through to the mod folder. The copied file must be a real file (not
// a symlink) with the exec bit set.
static void check_direct_executable_copy(const fs::path& root,
                                         const std::string& label) {
    const fs::path mods = root / "mods";
    const fs::path game_dir = root / "game";
    fs::create_directories(game_dir);
    const fs::path ledger = root / ".gmm_deploy_ledger";

    fs::create_directories(mods / "ToolMod" / "bin");
    write_file(mods / "ToolMod" / "bin" / "Tool.exe", "MZ");
    write_file(mods / "ToolMod" / "bin" / "data.txt", "x");

    require(engine::deploy_all_enabled_mods_direct(
                mods, game_dir, "Data", false, "disabled.txt", true, ledger),
            label + ": deploys");

    const fs::path exe_target = game_dir / "Data" / "bin" / "Tool.exe";
    require(fs::is_regular_file(exe_target) && !fs::is_symlink(exe_target),
            label + ": executable is a real copy, not a symlink");
    const auto perms = fs::status(exe_target).permissions();
    require((perms & fs::perms::owner_exec) != fs::perms::none,
            label + ": copied executable carries the exec bit");

    const fs::path data_target = game_dir / "Data" / "bin" / "data.txt";
    require(fs::is_symlink(data_target), label + ": non-executable stays a symlink");
}

// Original-file safety (Aug 2026): direct-symlink deploys must NEVER destroy a
// game-native file that a mod overrides. On first collision the original is
// moved to <game_dir>/Original_Files/<relative path>; a mod that stops being a
// winner restores it; remove_deployed_files() restores everything and drops the
// ledger; a fresh deploy re-backs-up the restored original.
static void check_direct_backup_restore(const fs::path& root,
                                        const std::string& label) {
    const fs::path mods = root / "mods";
    const fs::path game_dir = root / "game";
    const fs::path ledger = root / ".gmm_deploy_ledger";
    const fs::path originals = game_dir / "Original_Files";
    fs::create_directories(game_dir / "Data");
    write_file(game_dir / "Data" / "original.esp", "original");
    write_file(game_dir / "Data" / "native.esp", "native");

    auto deploy = [&]() {
        return engine::deploy_all_enabled_mods_direct(
            mods, game_dir, "Data", false, "disabled.txt", true, ledger,
            originals);
    };

    fs::create_directories(mods / "AMod");
    write_file(mods / "AMod" / "original.esp", "modded");
    write_file(mods / "AMod" / "newfile.esp", "new");

    require(deploy(), label + ": first deploy");
    const fs::path original_target = game_dir / "Data" / "original.esp";
    require(fs::is_symlink(original_target), label + ": mod wins the collided path");
    require(fs::read_symlink(original_target) == mods / "AMod" / "original.esp",
            label + ": collided path is the mod's link");
    const fs::path backup = originals / "Data" / "original.esp";
    require(fs::is_regular_file(backup), label + ": original backed up into Original_Files");
    {
        std::ifstream in(backup);
        std::string s;
        std::getline(in, s);
        require(s == "original", label + ": backup preserves original content");
    }

    // Idempotent redeploy must not back up twice or clobber the backup.
    require(deploy(), label + ": idempotent redeploy");
    require(fs::is_symlink(original_target), label + ": redeploy keeps the mod link");
    require(fs::is_regular_file(backup), label + ": backup intact after redeploy");

    // A mod that stops being a winner restores the original it displaced.
    write_file(mods / "AMod" / "disabled.txt", "x");
    require(deploy(), label + ": redeploy with AMod disabled");
    require(fs::is_regular_file(original_target) && !fs::is_symlink(original_target),
            label + ": disabled mod restores the original file");
    {
        std::ifstream in(original_target);
        std::string s;
        std::getline(in, s);
        require(s == "original", label + ": restored original content");
    }
    require(!fs::exists(backup), label + ": restored original left the backup store");
    require(!fs::exists(game_dir / "Data" / "newfile.esp"),
            label + ": disabled mod's unique file removed");

    // Re-enable, then a full teardown: remove_deployed_files() restores the
    // pristine state (originals back in place, mod artifacts gone, ledger gone).
    fs::remove(mods / "AMod" / "disabled.txt");
    require(deploy(), label + ": redeploy with AMod enabled again");
    require(fs::is_symlink(original_target), label + ": re-enabled mod wins again");
    require(fs::is_regular_file(backup), label + ": original backed up again");

    require(engine::remove_deployed_files(game_dir, originals, ledger),
            label + ": remove_deployed_files succeeds");
    require(fs::is_regular_file(original_target) && !fs::is_symlink(original_target),
            label + ": collided path restored as the original file");
    {
        std::ifstream in(original_target);
        std::string s;
        std::getline(in, s);
        require(s == "original", label + ": removed deploy leaves original content");
    }
    require(!fs::exists(game_dir / "Data" / "newfile.esp"),
            label + ": mod file removed by teardown");
    require(fs::is_regular_file(game_dir / "Data" / "native.esp"),
            label + ": untouched game file stays");
    require(!fs::exists(backup), label + ": backup store empty after teardown");
    require(!fs::exists(ledger), label + ": ledger dropped after full removal");

    // With the game pristine again a fresh deploy re-backs-up the original.
    require(deploy(), label + ": deploy after full removal");
    require(fs::is_symlink(original_target), label + ": post-removal deploy wins again");
    require(fs::is_regular_file(backup), label + ": post-removal deploy re-backs-up the original");
}

// Merged-view reachability (merged_view_file_exists): an executable is a valid
// launch target when it exists physically (native game file, live overlay
// mount, or a legacy absolute path already resolved into the mods folder) OR
// when it is a game-relative path whose deployed copy lives in .gmm_staging -
// even when reached through the ~/.steam symlink spelling of the game dir.
static void check_merged_view(const fs::path& base) {
    const fs::path game_dir = base / "game";
    const fs::path staging = base / ".gmm_staging";
    fs::create_directories(game_dir);
    fs::create_directories(staging);

    // 1) Native game file, physically present.
    const fs::path native = game_dir / "SkyrimSE.exe";
    write_file(native, "bin");
    require(engine::merged_view_file_exists(game_dir, staging, native),
            "merged_view: native file is reachable");

    // 2) Staged-only file (root-override mod exe deployed at the game root).
    const fs::path staged_only = game_dir / "skse64_loader.exe";
    require(!fs::exists(staged_only),
            "merged_view: staged-only file is absent physically");
    write_file(staging / "skse64_loader.exe", "bin");
    require(engine::merged_view_file_exists(game_dir, staging, staged_only),
            "merged_view: staged-only file reachable via staging");
    require(!engine::merged_view_file_exists(game_dir, {}, staged_only),
            "merged_view: staged-only file NOT reachable without staging");

    // 3) Symlink-spelling mismatch: reach the staged file through a symlink
    // to game_dir (the ~/.steam -> ~/.local/share/Steam case).
    const fs::path link_dir = base / "link";
    std::error_code ec;
    fs::create_directory_symlink(game_dir, link_dir, ec);
    if (!ec) {
        const fs::path via_link = link_dir / "skse64_loader.exe";
        require(engine::merged_view_file_exists(game_dir, staging, via_link),
                "merged_view: staged-only file reachable through the symlink spelling");
        require(engine::merged_view_file_resolve(game_dir, staging, via_link) ==
                    staging / "skse64_loader.exe",
                "merged_view: resolve returns the staged copy through the symlink spelling");
    }

    // 4) Legacy absolute path into the physical mods folder.
    const fs::path mods_exe =
        base / "mods" / "skse64_2_02_06" / "skse64_loader.exe";
    write_file(mods_exe, "bin");
    require(engine::merged_view_file_exists(game_dir, staging, mods_exe),
            "merged_view: legacy absolute mods path is reachable");

    // 5) Missing everywhere -> false.
    const fs::path missing = game_dir / "does_not_exist.exe";
    require(!engine::merged_view_file_exists(game_dir, staging, missing),
            "merged_view: missing file is not reachable");

    // 6) resolve(): the physical file wins, staging is the fallback, a total
    // miss returns empty - and the staging result matches the merged-view
    // semantics the overlay gate now uses (mount happens before execv).
    require(engine::merged_view_file_resolve(game_dir, staging, native) == native,
            "merged_view: resolve returns the physical file");
    require(engine::merged_view_file_resolve(game_dir, staging, staged_only) ==
                staging / "skse64_loader.exe",
            "merged_view: resolve falls back to the staged copy");
    require(engine::merged_view_file_resolve(game_dir, staging, missing).empty(),
            "merged_view: resolve is empty for a missing file");

    // 7) executable_reachable(): regular files only. A directory (e.g. a mod's
    // bin/ folder) resolves but must be rejected - execv'ing it would fail
    // cryptically inside the overlay child.
    require(engine::merged_view_executable_reachable(game_dir, staging, staged_only),
            "merged_view: staged file is executable-reachable");
    const fs::path staged_dir = staging / "Data";
    fs::create_directories(staged_dir);
    require(!engine::merged_view_executable_reachable(game_dir, staging, staged_dir),
            "merged_view: a directory is not executable-reachable");
    require(!engine::merged_view_executable_reachable(game_dir, staging, missing),
            "merged_view: missing file is not executable-reachable");
}

// The per-instance "deploy_strategy" override in instance.toml wins over the
// game's knowledge default; an empty/missing override (or no instance at all)
// falls back to the knowledge key.
static void check_effective_strategy(const fs::path& root) {
    fs::create_directories(root);
    engine::GameKnowledge k = make_knowledge(false, "overlayfs");
    require(engine::effective_deploy_strategy(root, k, "testgame") == "overlayfs",
            "effective strategy falls back to knowledge with no toml");

    engine::Instance inst = engine::Instance::from_root(root);
    require(inst.write_key("deploy_strategy", "symlink"),
            "write_key sets the deploy_strategy override");
    require(engine::effective_deploy_strategy(root, k, "testgame") == "symlink",
            "effective strategy honors the instance.toml override");

    require(inst.write_key("deploy_strategy", ""),
            "write_key clears the deploy_strategy override");
    require(engine::effective_deploy_strategy(root, k, "testgame") == "overlayfs",
            "effective strategy falls back after override cleared");

    require(engine::effective_deploy_strategy({}, k, "testgame") == "overlayfs",
            "effective strategy with empty instance root uses knowledge");
}

TEST_CASE("launch params", "[engine]") {
    const fs::path base =
        fs::current_path() / ("gmm_test_launch_params_" + std::to_string(getpid()));

    // Pure filesystem predicate: runs even where overlay deployment cannot.
    check_merged_view(base);

    check_effective_strategy(base / "instances" / "Effective");

    // Direct-symlink mode (the default deploy_strategy): no overlay needed.
    check_deploy_direct(base / "instances" / "DirectFlat", false, "direct flat deploy");
    check_deploy_direct(base / "instances" / "DirectByMod", true, "direct include-mod-id deploy");
    check_direct_winner_change(base / "instances" / "DirectWinner", "direct winner change");
    check_direct_executable_copy(base / "instances" / "DirectExe", "direct executable copy");
    check_direct_backup_restore(base / "instances" / "DirectBackup", "direct backup restore");

    // Overlay-mode checks: graceful skip on filesystems that cannot host an
    // overlay upperdir (kernel < 5.11 or no user xattr).
    if (!engine::OverlayFsLauncher::is_supported(base / "probe")) {
        SKIP("overlay not supported here");
    }

    check_deploy(base / "instances" / "Flat", false, "flat deploy");
    check_deploy(base / "instances" / "ByMod", true, "include-mod-id deploy");
    check_deploy_request(base / "instances" / "Request", "request overload");
    check_ci_deploy(base / "instances" / "CI", "ci deploy");

    fs::remove_all(base);
}
