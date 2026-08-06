// Engine regression test for case-insensitive deploy (Windows games).
//
// XP32's FOMOD ships a `Meshes/` dir AND a `meshes/` dir (different files
// split between them). The deploy walk preserves each mod's on-disk casing, so
// the case-sensitive overlay staging tree would expose both dirs and the game
// (case-insensitive by nature) reads a half-populated mod. When the per-game
// `case_sensitive` knowledge flag is false, every deploy target is routed
// through resolve_deploy_target_ci and CI-equal directory paths collapse into
// one canonical casing, AND the staged tree is made reachable under the
// lowercase spellings the game actually queries (add_case_insensitive_aliases
// creates `meshes` -> `Meshes` style symlinks plus the root `data` -> `Data`
// alias). Pinned here:
//
//   1. same-mod merge: `Meshes/a.nif` + `meshes/b.nif` land in ONE real dir.
//   2. cross-mod merge: mod A `Meshes/x`, mod B `meshes/y` share one dir.
//   3. lowercase alias: the non-canonical spelling resolves to the same files
//      (`data/meshes/...` and `data` root alias both work).
//   4. recursion: a deep uppercase dir (Meshes/UppercaseSub) gets its own
//      lowercase alias at every level.
//   5. control: case_sensitive=true keeps both casings and creates no aliases.
//   6. hidden files are still skipped under the merge.
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

// True only for a REAL directory - symlinks (aliases, staged file links) do
// not count.
static bool is_real_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(fs::symlink_status(p, ec));
}

static void deploy_and_merge(const fs::path& base, bool case_sensitive,
                             const char* label) {
    const fs::path root = base / "instance";
    const fs::path mods = root / "mods";

    // Same-mod split: one file under `Meshes`, one under `meshes`.
    write_file(mods / "ModA" / "Meshes" / "a.nif", "x");
    write_file(mods / "ModA" / "meshes" / "b.nif", "x");

    // Deep uppercase dir to exercise recursive aliasing.
    write_file(mods / "ModA" / "Meshes" / "UppercaseSub" / "deep.nif", "x");
    // Already-lowercase dir: no alias may be created for it.
    write_file(mods / "ModA" / "Meshes" / "lowercase" / "plain.nif", "x");

    // Cross-mod split: a second mod that only carries the lowercase spelling.
    write_file(mods / "ModB" / "meshes" / "c.nif", "x");

    // Hidden file must stay skipped even under the merge.
    write_file(mods / "ModA" / "Meshes" / "Hidden.nif.gmmhidden", "x");

    const fs::path staging = root / ".gmm_staging";
    const bool ok = engine::deploy_all_enabled_mods(
        mods, staging, "Data", /*deploy_include_mod_id=*/false, "",
        case_sensitive);
    check(ok, std::string(label).append(": deploy succeeds").c_str());

    const std::string l = label;
    if (case_sensitive) {
        check(is_real_dir(staging / "Data" / "Meshes") &&
                  is_real_dir(staging / "Data" / "meshes"),
              (l + ": control keeps both casings (Meshes + meshes)").c_str());
        check(fs::exists(staging / "Data" / "Meshes" / "a.nif") &&
                  fs::exists(staging / "Data" / "meshes" / "b.nif"),
              (l + ": control keeps each file under its own casing").c_str());
        check(!fs::is_symlink(staging / "data") &&
                  !fs::exists(staging / "data"),
              (l + ": control creates no root data alias").c_str());
    } else {
        // Exactly ONE real directory among the CI-equal spellings; the other
        // may only exist as a lowercase symlink alias.
        const bool real_upper = is_real_dir(staging / "Data" / "Meshes");
        const bool real_lower = is_real_dir(staging / "Data" / "meshes");
        check(real_upper != real_lower,
              (l + ": CI-equal dirs merge into exactly one real casing").c_str());
        const fs::path dir =
            real_upper ? staging / "Data" / "Meshes" : staging / "Data" / "meshes";

        check(fs::exists(dir / "a.nif") && fs::exists(dir / "b.nif") &&
                  fs::exists(dir / "c.nif"),
              (l + ": merged dir carries every file (same-mod + cross-mod)").c_str());
        check(fs::exists(dir / "UppercaseSub" / "deep.nif"),
              (l + ": deep subdir survives the merge").c_str());

        // The non-canonical spelling resolves through the alias symlink.
        const fs::path alias = real_upper ? staging / "Data" / "meshes"
                                          : staging / "Data" / "Meshes";
        check(fs::is_symlink(alias),
              (l + ": non-canonical spelling is a lowercase alias symlink").c_str());
        check(fs::exists(alias / "a.nif") && fs::exists(alias / "b.nif") &&
                  fs::exists(alias / "c.nif"),
              (l + ": files are reachable through the lowercase alias").c_str());
        check(fs::exists(alias / "UppercaseSub" / "deep.nif") &&
                  fs::is_symlink(alias / "uppercasesub") &&
                  fs::exists(alias / "uppercasesub" / "deep.nif"),
              (l + ": deep dir has its own lowercase alias at every level").c_str());

        // Root-level `data` -> `Data` alias (games resolve "data/..." relative
        // to their cwd == the overlay root).
        check(fs::is_symlink(staging / "data"),
              (l + ": root data -> Data alias exists").c_str());
        check(fs::exists(staging / "data" / "meshes" / "a.nif") &&
                  fs::exists(staging / "data" / "Meshes" / "b.nif"),
              (l + ": files resolve through the root data alias").c_str());

        // No alias for an already-lowercase dir name.
        const fs::path real_dir_for_lower =
            real_upper ? staging / "Data" / "Meshes" : staging / "Data" / "meshes";
        check(!fs::is_symlink(real_dir_for_lower / "lowercase"),
              (l + ": no alias created for an already-lowercase dir").c_str());
        check(fs::exists(real_dir_for_lower / "lowercase" / "plain.nif"),
              (l + ": lowercase dir content survives").c_str());

        check(!fs::exists(dir / "Hidden.nif.gmmhidden"),
              (l + ": hidden file still skipped under the merge").c_str());
        check(!fs::exists(staging / "Data" / "Meshes" / "Hidden.nif.gmmhidden") &&
                  !fs::exists(staging / "Data" / "meshes" / "Hidden.nif.gmmhidden"),
              (l + ": no hidden file anywhere in the staging tree").c_str());
    }
}

int main() {
    const fs::path base =
        fs::current_path() / ("gmm_deploy_ci_" + std::to_string(getpid()));
    fs::remove_all(base);
    fs::create_directories(base);

    deploy_and_merge(base / "Merge", /*case_sensitive=*/false, "merge");
    deploy_and_merge(base / "Control", /*case_sensitive=*/true, "control");

    fs::remove_all(base);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
