// Engine regression test for the parallel deploy executor (P8.4, PLAN §13.3).
//
// Pins four contracts the parallel launch deploy depends on:
//   1. A first-ever full deploy across many mods produces the same tree as the
//      sequential executor while farming the work across N threads, and
//      reports monotonic progress that completes at the total.
//   2. A contested target (two enabled mods shipping the same relative path)
//      is won deterministically by the LAST mod in lexicographic folder order
//      (the sequential executor let directory_iterator order decide — arbitrary
//      filesystem order).
//   3. Incremental redeploys stay O(Δ): an unchanged re-run touches zero files
//      (ledger diff), and disabling a mod unlinks its now-stale staged files
//      (re-pointing a contested target to its new winner).
//   4. The case-insensitive merge (Meshes/ vs meshes/ -> one casing) still
//      holds under parallelism.
#include "engine/deploy/deploy_utils.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

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

int main() {
    const fs::path base =
        fs::current_path() / ("gmm_test_deploy_parallel_" + std::to_string(getpid()));
    const fs::path mods = base / "mods";
    const fs::path staging = base / "staging";

    // --- 1) Parallel full deploy: 8 mods x (5 root files + 5 nested) of
    // DISTINCT relative paths, 4 threads. All 80 files must land as symlinks.
    for (int m = 0; m < 8; ++m) {
        const std::string name = "Mod" + std::to_string(m);
        for (int f = 0; f < 5; ++f) {
            write_file(mods / name / (name + "_file" + std::to_string(f) + ".txt"), "x");
            write_file(mods / name / "Sub" / (name + "_s" + std::to_string(f) + ".txt"), "x");
        }
    }

    std::vector<std::pair<int, int>> progress_calls;
    bool ok = engine::deploy_all_enabled_mods_parallel(
        mods, staging, "Data", false, "", true, 4,
        [&progress_calls](int done, int total) {
            progress_calls.emplace_back(done, total);
        });
    check(ok, "parallel full deploy succeeds");
    for (int m = 0; m < 8; ++m) {
        const std::string name = "Mod" + std::to_string(m);
        for (int f = 0; f < 5; ++f) {
            check(fs::is_symlink(staging / "Data" / (name + "_file" + std::to_string(f) + ".txt")),
                  (name + " root file deployed").c_str());
            check(fs::is_symlink(staging / "Data" / "Sub" / (name + "_s" + std::to_string(f) + ".txt")),
                  (name + " nested file deployed").c_str());
        }
    }

    // Progress: every reported total is 80, done is monotonic and reaches it.
    bool monotonic = true;
    int last = 0;
    int final_total = -1;
    for (const auto& [d, t] : progress_calls) {
        if (t != 80) { final_total = -2; break; }
        if (d < last || d > t) monotonic = false;
        last = d;
        final_total = t;
    }
    check(progress_calls.size() > 1, "progress reported incrementally");
    check(monotonic, "progress is monotonic");
    check(final_total == 80, "progress total is the full work count");
    check(last == 80, "progress completes at the total");

    // --- 2) Deterministic conflict winner: ModZ and ModA both ship
    // conflict.txt; ModZ also ships a unique zonly.txt. Last lexicographic
    // folder wins -> ModZ.
    write_file(mods / "ModZ" / "conflict.txt", "zzz");
    write_file(mods / "ModA" / "conflict.txt", "aaa");
    write_file(mods / "ModZ" / "zonly.txt", "x");
    ok = engine::deploy_all_enabled_mods_parallel(mods, staging, "Data", false, "", true, 4);
    check(ok, "conflict redeploy succeeds");
    check(fs::is_symlink(staging / "Data" / "conflict.txt"), "contested target deployed");
    if (fs::is_symlink(staging / "Data" / "conflict.txt"))
        check(fs::read_symlink(staging / "Data" / "conflict.txt") ==
                  mods / "ModZ" / "conflict.txt",
              "lexicographically-last mod wins the contested target");
    check(fs::is_symlink(staging / "Data" / "zonly.txt"), "unique file of the winner deployed");

    // --- 3a) O(Δ) ledger: an unchanged re-run touches zero files.
    progress_calls.clear();
    ok = engine::deploy_all_enabled_mods_parallel(
        mods, staging, "Data", false, "", true, 4,
        [&progress_calls](int done, int total) {
            progress_calls.emplace_back(done, total);
        });
    check(ok, "unchanged redeploy succeeds");
    check(progress_calls.size() == 1 && progress_calls.front().second == 0,
          "unchanged redeploy touches zero files (ledger O(Δ))");
    check(fs::is_symlink(staging / "Data" / "conflict.txt"),
          "staged tree intact after unchanged redeploy");

    // --- 3b) Disabling a mod unlinks its stale files and re-points the
    // contested target to the new winner.
    write_file(mods / "ModZ" / ".gmmdisabled", "x");
    ok = engine::deploy_all_enabled_mods_parallel(
        mods, staging, "Data", false, ".gmmdisabled", true, 4);
    check(ok, "redeploy after disable succeeds");
    check(!fs::exists(staging / "Data" / "zonly.txt"),
          "disabled mod's unique file unlinked (no stale staging entries)");
    check(!fs::exists(staging / "Data" / "ModZ_file0.txt"),
          "disabled mod's distinct files unlinked");
    if (fs::is_symlink(staging / "Data" / "conflict.txt"))
        check(fs::read_symlink(staging / "Data" / "conflict.txt") ==
                  mods / "ModA" / "conflict.txt",
              "contested target re-pointed to the new winner");
    check(fs::is_symlink(staging / "Data" / "Mod0_file0.txt"),
          "remaining mods' files untouched");

    // --- 3c) Re-enabling brings the mod back (new winner again).
    fs::remove(mods / "ModZ" / ".gmmdisabled");
    ok = engine::deploy_all_enabled_mods_parallel(mods, staging, "Data", false, "", true, 4);
    check(ok, "redeploy after re-enable succeeds");
    check(fs::is_symlink(staging / "Data" / "zonly.txt"), "re-enabled mod's file back");
    if (fs::is_symlink(staging / "Data" / "conflict.txt"))
        check(fs::read_symlink(staging / "Data" / "conflict.txt") ==
                  mods / "ModZ" / "conflict.txt",
              "contested target won by the re-enabled mod again");

    // --- 4) Include-mod-id layout under parallelism: each mod gets its own
    // Data/<folder>/ subtree, so even contested paths coexist.
    const fs::path staging_by_mod = base / "staging_bymod";
    ok = engine::deploy_all_enabled_mods_parallel(
        mods, staging_by_mod, "Data", true, "", true, 4);
    check(ok, "include-mod-id parallel deploy succeeds");
    check(fs::is_symlink(staging_by_mod / "Data" / "ModA" / "conflict.txt"),
          "include-mod-id keeps both contesting files");
    check(fs::is_symlink(staging_by_mod / "Data" / "ModZ" / "conflict.txt"),
          "include-mod-id keeps both contesting files (2)");

    // --- 5) Case-insensitive merge under parallelism: Meshes/ and meshes/
    // collapse into one on-disk casing.
    const fs::path ci_mods = base / "ci_mods";
    const fs::path ci_staging = base / "ci_staging";
    write_file(ci_mods / "CI_Mod" / "Meshes" / "a.nif", "x");
    write_file(ci_mods / "CI_Mod" / "meshes" / "b.nif", "x");
    ok = engine::deploy_all_enabled_mods_parallel(
        ci_mods, ci_staging, "Data", false, "", false, 4);
    check(ok, "parallel CI deploy succeeds");
    const fs::path data = ci_staging / "Data";
    const bool has_upper = fs::exists(data / "Meshes");
    const bool has_lower = fs::exists(data / "meshes");
    check(has_upper != has_lower, "CI-equal dirs merge into exactly one casing");
    const fs::path merged = has_upper ? data / "Meshes" : data / "meshes";
    check(fs::is_symlink(merged / "a.nif") && fs::is_symlink(merged / "b.nif"),
          "both spellings' files deployed into the merged casing");

    fs::remove_all(base);
    std::printf("%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
