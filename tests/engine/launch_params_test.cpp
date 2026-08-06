// Engine regression test pinning the launch-deploy contract.
//
// GUI and CLI game launches MUST go through engine::prepare_launch_params:
// it deploys all enabled mods into <instance>/.gmm_staging and adds that dir
// to LaunchParams::extra_lowerdirs on EVERY launch. A previous GUI-only
// regression skipped the deploy and merely reused whatever was already in
// .gmm_staging (which the watchdog wipes at every session end), so launches
// after the first game session saw no mods at all. This test pins the
// contract: deploy happens, disabled mods and special dirs are skipped, the
// deploy is idempotent, and staging is the overlay lowerdir.
#include "engine/instance/instance_utils.h"
#include "engine/fs_utils.h"
#include "engine/launcher.h"
#include "engine/overlay_launcher.h"
#include "engine/registry/game_knowledge.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

static void require(bool cond, const std::string& msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
        std::exit(1);
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

static engine::GameKnowledge make_knowledge(bool include_mod_id) {
    engine::GameKnowledge k;
    k.set("testgame", "deploy_prefix", "Data");
    k.set("testgame", "deploy_include_mod_id", include_mod_id ? "true" : "false");
    k.set("testgame", "disable_mechanism", "disabled.txt");
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

    std::vector<std::pair<int, int>> progress;
    const auto params = engine::prepare_launch_params(
        req, [&progress](int done, int total) {
            progress.emplace_back(done, total);
        });

    require(params.extra_lowerdirs.size() == 1,
            label + ": request overload yields one lowerdir");
    const fs::path deployed = root / ".gmm_staging" / "Data" / "RaceMenu.esp";
    require(fs::is_symlink(deployed), label + ": deploy finished before return");

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

int main() {
    const fs::path base =
        fs::current_path() / ("gmm_test_launch_params_" + std::to_string(getpid()));

    // Pure filesystem predicate: runs even where overlay deployment cannot.
    check_merged_view(base);

    // Graceful skip on filesystems that cannot host an overlay upperdir
    // (kernel < 5.11 or no user xattr): the deploy branch can't run there.
    if (!engine::OverlayFsLauncher::is_supported(base / "probe")) {
        std::printf("launch_params_test: overlay not supported here - skipping\n");
        fs::remove_all(base);
        return 0;
    }

    check_deploy(base / "instances" / "Flat", false, "flat deploy");
    check_deploy(base / "instances" / "ByMod", true, "include-mod-id deploy");
    check_deploy_request(base / "instances" / "Request", "request overload");
    check_ci_deploy(base / "instances" / "CI", "ci deploy");

    fs::remove_all(base);
    std::printf("launch_params_test: all checks passed\n");
    return 0;
}
