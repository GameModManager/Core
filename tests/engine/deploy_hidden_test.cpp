// Engine regression test for the MO2-parity hidden-file contract.
//
// Hiding a file renames it to `<name>.gmmhidden` (GMM's marker) - and
// recognizes MO2's `.mohidden` suffix from imported instances. Two behaviors
// are pinned:
//
//   1. fs_utils helpers: is_hidden_file() recognizes both suffixes,
//      hide_file() renames to `.gmmhidden`, unhide_file() strips whichever
//      suffix is present (GMM or MO2) and is a no-op on visible files.
//
//   2. deploy_all_enabled_mods() must NOT deploy hidden files: a hidden file
//      can never reach the game, matching MO2's renamed-suffix convention.
//      Both the GMM and MO2 suffixes are covered, at the top level and nested
//      in subdirectories, and a visible sibling still deploys.
#include "engine/fs_utils.h"
#include "engine/deploy/deploy_utils.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

static int failures = 0;
static int passes = 0;
static void check(bool cond, const char* what) {
    std::printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
    if (cond)
        ++passes;
    else
        ++failures;
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

static void make_instance(const fs::path& root) {
    // Enabled mod with a visible file, a GMM-hidden file, an MO2-hidden file,
    // and the same mix nested one level deep. Files sit directly in the mod
    // folder (MO2 layout: the folder root IS the game's Data root), so
    // deploy_prefix="Data" maps them to staging/Data/<rel>.
    const fs::path mod = root / "mods" / "EnabledMod";
    write_file(mod / "SkyUI.esp", "x");
    write_file(mod / "Hidden.ini.gmmhidden", "x");
    write_file(mod / "HiddenMo2.ini.mohidden", "x");
    write_file(mod / "Sub" / "nested.txt.gmmhidden", "x");
    write_file(mod / "Sub" / "nested_mo2.txt.mohidden", "x");
    write_file(mod / "Sub" / "nested.txt", "x");

    // Disabled mod: carries the .gmmdisabled sentinel at its root, so with
    // disable_mechanism=".gmmdisabled" the whole mod must be skipped.
    const fs::path disabled = root / "mods" / "DisabledMod";
    write_file(disabled / "DisabledMod.esp", "x");
    write_file(disabled / ".gmmdisabled", "");

    // Root-override mod (meta.ini [General] rootOverride=1): top-level files
    // deploy to the staging root (the game root), a leading Data/ folder still
    // lands in Data, and hidden files stay hidden.
    const fs::path root_mod = root / "mods" / "RootMod";
    write_file(root_mod / "meta.ini", "[General]\nrootOverride = 1\n");
    write_file(root_mod / "RootHook.dll", "x");
    write_file(root_mod / "Data" / "meshes" / "root.nif", "x");
    write_file(root_mod / "Data" / "hidden.txt.gmmhidden", "x");

    // Disabled root-override mod: the sentinel must win over the flag.
    const fs::path disabled_root = root / "mods" / "DisabledRootMod";
    write_file(disabled_root / "meta.ini", "[General]\nrootOverride = 1\n");
    write_file(disabled_root / "RootHook2.dll", "x");
    write_file(disabled_root / ".gmmdisabled", "");
}

int main() {
    // --- fs_utils hide/unhide/is_hidden contract ---
    check(engine::is_hidden_file("Data/Hidden.ini.gmmhidden"),
          "is_hidden_file recognizes the .gmmhidden suffix");
    check(engine::is_hidden_file("Data/Hidden.ini.mohidden"),
          "is_hidden_file recognizes the .mohidden suffix");
    check(!engine::is_hidden_file("Data/SkyUI.esp"),
          "is_hidden_file rejects visible files");
    check(!engine::is_hidden_file("Data/Hidden.ini.gmmhidden.txt"),
          "is_hidden_file rejects a suffix look-alike");

    const fs::path dir = fs::current_path() / ("gmm_hide_utils_" + std::to_string(getpid()));
    fs::create_directories(dir);

    write_file(dir / "plain.txt", "x");
    check(engine::hide_file(dir / "plain.txt"), "hide_file renames a visible file");
    check(fs::exists(dir / "plain.txt.gmmhidden") && !fs::exists(dir / "plain.txt"),
          "hide_file produces <name>.gmmhidden and removes the original");
    check(engine::hide_file(dir / "plain.txt.gmmhidden"),
          "hide_file on an already-hidden file is a no-op success");
    check(engine::unhide_file(dir / "plain.txt.gmmhidden"),
          "unhide_file strips the GMM suffix");
    check(fs::exists(dir / "plain.txt") && !fs::exists(dir / "plain.txt.gmmhidden"),
          "unhide_file restores the original name");

    write_file(dir / "mo2.txt.mohidden", "x");
    check(engine::unhide_file(dir / "mo2.txt.mohidden"),
          "unhide_file strips the MO2 suffix (MO2-imported instances)");
    check(fs::exists(dir / "mo2.txt") && !fs::exists(dir / "mo2.txt.mohidden"),
          "unhide_file restores the base name from .mohidden");
    check(engine::unhide_file(dir / "mo2.txt"),
          "unhide_file on a visible file is a no-op success");
    check(fs::exists(dir / "mo2.txt"),
          "no-op unhide leaves the file in place");
    fs::remove_all(dir);

    // --- deploy skips hidden files ---
    const fs::path base =
        fs::current_path() / ("gmm_deploy_hidden_" + std::to_string(getpid()));
    const fs::path root = base / "instance";
    make_instance(root);

    const fs::path staging = root / ".gmm_staging";
    const bool ok = engine::deploy_all_enabled_mods(
        root / "mods", staging, "Data", /*deploy_include_mod_id=*/false, "");
    check(ok, "deploy_all_enabled_mods succeeds");

    check(fs::exists(staging / "Data" / "SkyUI.esp"),
          "visible file is deployed");
    check(!fs::exists(staging / "Data" / "Hidden.ini.gmmhidden"),
          "GMM-hidden file is not deployed");
    check(!fs::exists(staging / "Data" / "HiddenMo2.ini.mohidden"),
          "MO2-hidden file is not deployed");
    check(fs::exists(staging / "Data" / "Sub" / "nested.txt"),
          "visible file nested in a subdirectory is deployed");
    check(!fs::exists(staging / "Data" / "Sub" / "nested.txt.gmmhidden"),
          "nested GMM-hidden file is not deployed");
    check(!fs::exists(staging / "Data" / "Sub" / "nested_mo2.txt.mohidden"),
          "nested MO2-hidden file is not deployed");

    // --- root-override mods deploy to the game root ---
    check(fs::exists(staging / "RootHook.dll"),
          "root-override mod's top-level file deploys to the game root");
    check(fs::exists(staging / "Data" / "meshes" / "root.nif"),
          "root-override mod's Data/ content still lands in Data");
    check(!fs::exists(staging / "Data" / "RootHook.dll"),
          "root-override top-level file does not leak under Data");
    check(!fs::exists(staging / "Data" / "hidden.txt.gmmhidden"),
          "hidden file of a root-override mod is not deployed");

    // --- deploy skips disabled mods (disable sentinel) ---
    const fs::path staging2 = root / ".gmm_staging2";
    const bool ok2 = engine::deploy_all_enabled_mods(
        root / "mods", staging2, "Data", /*deploy_include_mod_id=*/false,
        ".gmmdisabled");
    check(ok2, "deploy with disable mechanism succeeds");
    check(!fs::exists(staging2 / "Data" / "DisabledMod.esp"),
          "disabled mod's files are not deployed");
    check(!fs::exists(staging2 / "Data" / ".gmmdisabled"),
          "disable sentinel is not deployed");
    check(fs::exists(staging2 / "Data" / "SkyUI.esp"),
          "enabled mod still deployed with mechanism set");
    check(!fs::exists(staging2 / "RootHook2.dll"),
          "disabled root-override mod's root files are not deployed");

    fs::remove_all(base);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
