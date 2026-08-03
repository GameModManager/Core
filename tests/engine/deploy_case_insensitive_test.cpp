// Engine regression test for case-insensitive deploy (Windows games).
//
// XP32's FOMOD ships a `Meshes/` dir AND a `meshes/` dir (different files
// split between them). The deploy walk preserves each mod's on-disk casing, so
// the case-sensitive overlay staging tree would expose both dirs and the game
// (case-insensitive by nature) reads a half-populated mod. When the per-game
// `case_sensitive` knowledge flag is false, every deploy target is routed
// through resolve_deploy_target_ci and CI-equal directory paths collapse into
// one canonical casing. Pinned here:
//
//   1. same-mod merge: `Meshes/a.nif` + `meshes/b.nif` land in ONE dir.
//   2. cross-mod merge: mod A `Meshes/x`, mod B `meshes/y` share one dir.
//   3. control: case_sensitive=true keeps both casings (today's behavior).
//   4. hidden files are still skipped under the merge.
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

// True when exactly one of the two CI-equal names exists; returns that dir.
static fs::path one_of(const fs::path& parent,
                       const std::string& upper, const std::string& lower,
                       bool& ok) {
    const bool has_upper = fs::exists(parent / upper);
    const bool has_lower = fs::exists(parent / lower);
    ok = has_upper != has_lower;
    return has_upper ? parent / upper : parent / lower;
}

static void deploy_and_merge(const fs::path& base, bool case_sensitive,
                             const char* label) {
    const fs::path root = base / "instance";
    const fs::path mods = root / "mods";

    // Same-mod split: one file under `Meshes`, one under `meshes`.
    write_file(mods / "ModA" / "Meshes" / "a.nif", "x");
    write_file(mods / "ModA" / "meshes" / "b.nif", "x");

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
        check(fs::exists(staging / "Data" / "Meshes") &&
                  fs::exists(staging / "Data" / "meshes"),
              (l + ": control keeps both casings (Meshes + meshes)").c_str());
        check(fs::exists(staging / "Data" / "Meshes" / "a.nif") &&
                  fs::exists(staging / "Data" / "meshes" / "b.nif"),
              (l + ": control keeps each file under its own casing").c_str());
    } else {
        bool merged = false;
        const fs::path dir =
            one_of(staging / "Data", "Meshes", "meshes", merged);
        check(merged, (l + ": CI-equal dirs merge into exactly one casing").c_str());
        check(fs::exists(dir / "a.nif") && fs::exists(dir / "b.nif") &&
                  fs::exists(dir / "c.nif"),
              (l + ": merged dir carries every file (same-mod + cross-mod)").c_str());
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
