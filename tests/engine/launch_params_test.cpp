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
#include "engine/launcher.h"
#include "engine/overlay_launcher.h"
#include "engine/registry/game_knowledge.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

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

int main() {
    const fs::path base =
        fs::current_path() / ("gmm_test_launch_params_" + std::to_string(getpid()));

    // Graceful skip on filesystems that cannot host an overlay upperdir
    // (kernel < 5.11 or no user xattr): the deploy branch can't run there.
    if (!engine::OverlayFsLauncher::is_supported(base / "probe")) {
        std::printf("launch_params_test: overlay not supported here - skipping\n");
        fs::remove_all(base);
        return 0;
    }

    check_deploy(base / "instances" / "Flat", false, "flat deploy");
    check_deploy(base / "instances" / "ByMod", true, "include-mod-id deploy");

    fs::remove_all(base);
    std::printf("launch_params_test: all checks passed\n");
    return 0;
}
