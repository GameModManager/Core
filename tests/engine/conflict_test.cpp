#include "engine/index/conflict_engine.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

#if defined(__unix__)
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>
#endif

namespace fs = std::filesystem;

namespace {
void check(bool cond, const char* what) {
    INFO(what);
    REQUIRE(cond);
}
}

TEST_CASE("conflict", "[engine]") {
    using namespace engine;

    fs::path base = fs::temp_directory_path() / "conflict_test_core";
    fs::remove_all(base);
    fs::create_directories(base / "mod_a" / "data");
    fs::create_directories(base / "mod_b" / "data");
    fs::create_directories(base / "mod_c" / "data");

    auto touch = [](const fs::path& p) { std::ofstream(p).put('\n'); };

    touch(base / "mod_a" / "data" / "config.ini");
    touch(base / "mod_b" / "data" / "config.ini");
    touch(base / "mod_c" / "data" / "config.ini");

    touch(base / "mod_a" / "unique_a.txt");
    touch(base / "mod_b" / "unique_b.txt");
    touch(base / "mod_c" / "unique_c.txt");

    touch(base / "mod_a" / "metadata.xml");

    ConflictEngine engine;

    std::vector<ConflictEngine::ModInfo> mods = {
        {"mod_a", 1},
        {"mod_b", 2},
        {"mod_c", 3},
    };

    // --- conflict_reversed=true: lower priority number wins (Isaac) ---
    {
        auto results = engine.compute(base, mods, "", "metadata.xml", true);

        check(results.size() == 3, "compute returns one entry per mod");
        check(results["mod_a"].total_files == 2, "mod_a indexes its two files");
        check(results["mod_a"].wins == 1, "mod_a (pri=1) wins the shared config.ini");
        check(results["mod_a"].losses == 0, "mod_a has no losses");
        check(results["mod_b"].total_files == 2, "mod_b indexes its two files");
        check(results["mod_b"].wins == 0, "mod_b (pri=2) does not win config.ini");
        check(results["mod_b"].losses == 1, "mod_b loses config.ini to mod_a");
        check(results["mod_c"].total_files == 2, "mod_c indexes its two files");
        check(results["mod_c"].wins == 0, "mod_c (pri=3) does not win config.ini");
        check(results["mod_c"].losses == 1, "mod_c loses config.ini to mod_a");

        std::printf("  conflict_reversed=true: mod_a (pri=1) wins config.ini\n");
    }

    // --- conflict_reversed=false: higher priority number wins (MO2) ---
    {
        auto results = engine.compute(base, mods, "", "metadata.xml", false);

        check(results.size() == 3, "compute returns one entry per mod");
        check(results["mod_c"].total_files == 2, "mod_c indexes its two files");
        check(results["mod_c"].wins == 1, "mod_c (pri=3) wins the shared config.ini");
        check(results["mod_c"].losses == 0, "mod_c has no losses");
        check(results["mod_b"].total_files == 2, "mod_b indexes its two files");
        check(results["mod_b"].wins == 0, "mod_b (pri=2) does not win config.ini");
        check(results["mod_b"].losses == 1, "mod_b loses config.ini to mod_c");
        check(results["mod_a"].total_files == 2, "mod_a indexes its two files");
        check(results["mod_a"].wins == 0, "mod_a (pri=1) does not win config.ini");
        check(results["mod_a"].losses == 1, "mod_a loses config.ini to mod_c");

        std::printf("  conflict_reversed=false: mod_c (pri=3) wins config.ini\n");
    }

    // --- Subdirectory renames (e.g. hiding via .gmmhidden) don't change the
    // mod root's quick token, so the cached file list would go stale and the
    // registry would keep the pre-rename path. invalidate_mod() must force a
    // re-scan so the next compute picks up the rename (MainWindow calls it
    // after every hide/un-hide for exactly this reason).
    {
        fs::path cache = base / "conflict_cache.json";
        engine.compute(base, mods, "", "metadata.xml", false, cache);
        fs::rename(base / "mod_c" / "data" / "config.ini",
                   base / "mod_c" / "data" / "config.ini.gmmhidden");
        engine.invalidate_mod("mod_c", cache);
        engine.compute(base, mods, "", "metadata.xml", false, cache);

        const auto& reg = engine.last_registry();
        auto it = reg.find("data/config.ini");
        check(it != reg.end() && it->second.size() == 2,
              "mod_c's hidden copy must not stay among config.ini's owners");
        check(reg.find("data/config.ini.gmmhidden") != reg.end(),
              "the renamed hidden file must appear as its own registry key");

        // Restore for the directory cleanup below.
        fs::rename(base / "mod_c" / "data" / "config.ini.gmmhidden",
                   base / "mod_c" / "data" / "config.ini");
        std::printf("  invalidate_mod after a subdir rename re-scans the mod\n");
    }

    // --- A permission-denied subdirectory inside a mod must not abort the
    // scan. Before the tree-based walker (DirectoryFileTree do_populate
    // increments with ec), the throwing operator++ terminated the whole
    // process (SIGABRT) on the first unreadable subdir. This is the
    // regression for the app crash "cannot increment recursive directory
    // iterator: Permission denied".
#if defined(__unix__)
    if (::geteuid() != 0) {
        fs::path locked_mod = base / "mod_locked";
        fs::create_directories(locked_mod / "data");
        fs::create_directories(locked_mod / "secret");
        touch(locked_mod / "data" / "config.ini");
        touch(locked_mod / "secret" / "hidden.bin");
        fs::permissions(locked_mod / "secret", fs::perms::none);

        std::vector<ConflictEngine::ModInfo> mods2 = mods;
        mods2.push_back({"mod_locked", 4});

        auto results = engine.compute(base, mods2, "", "metadata.xml", true);

        // The visible data/config.ini must still be indexed; the unreadable
        // secret/ subtree is skipped, not a crash.
        check(results["mod_locked"].total_files == 1,
              "visible file of the mod with an unreadable subdir is indexed");
        const auto& reg = engine.last_registry();
        auto it = reg.find("data/config.ini");
        check(it != reg.end() && it->second.size() == 4,
              "all four mods still own config.ini, locked or not");
        check(reg.find("secret/hidden.bin") == reg.end(),
              "unreadable subdir contents are not indexed");

        // Restore permissions so cleanup can remove the tree.
        fs::permissions(locked_mod / "secret", fs::perms::owner_all);
        fs::remove_all(locked_mod);
        std::printf("  permission-denied subdir is skipped, not a crash\n");
    } else {
        std::printf("  (skipped permission-denied check: running as root)\n");
    }

    // --- A symlinked subdirectory is not descended into, matching the legacy
    // recursive_directory_iterator (and MO2's QDir NoSymLinks filter). A
    // symlink to a FILE still counts as a file, exactly like the legacy
    // is_regular_file which follows the link.
    {
        fs::create_directories(base / "mod_link" / "data");
        fs::create_directories(base / "outside");
        touch(base / "mod_link" / "data" / "config.ini");
        touch(base / "outside" / "hidden.bin");

        std::error_code ec;
        fs::create_directory_symlink(base / "outside", base / "mod_link" / "evil", ec);
        fs::create_symlink(base / "outside" / "hidden.bin",
                           base / "mod_link" / "data" / "jump.ini", ec);
        if (ec) {
            std::printf("  (skipped symlink checks: symlinks unsupported)\n");
        } else {
            std::vector<ConflictEngine::ModInfo> sym_mods = {{"mod_link", 1}};
            auto results = engine.compute(base, sym_mods, "", "metadata.xml", false);

            check(results["mod_link"].total_files == 2,
                  "symlinked-dir contents not indexed, symlinked file is");
            const auto& reg = engine.last_registry();
            check(reg.find("data/config.ini") != reg.end(),
                  "regular file of the mod is indexed");
            check(reg.find("data/jump.ini") != reg.end(),
                  "symlink to a file is indexed as a file");
            check(reg.find("evil/hidden.bin") == reg.end(),
                  "symlinked subdirectory is not descended into");
            std::printf("  symlinked subdir is skipped, symlinked file kept\n");
        }
        fs::remove_all(base / "mod_link");
        fs::remove_all(base / "outside");
    }
#endif

    // --- CI-normalized registry keys: a Windows game's mods can ship both
    // casings of a directory (XP32: Meshes/ + meshes/). The deploy merges them
    // into one staged path, so the registry must register the two spellings as
    // ONE key (directory components lowercased) - otherwise the Data tab shows
    // the same logical file twice and conflicts are missed.
    {
        fs::path ci_mods = base / "mods_ci";
        fs::create_directories(ci_mods / "mod_upper" / "Meshes");
        fs::create_directories(ci_mods / "mod_lower" / "meshes");
        fs::create_directories(ci_mods / "mod_upper" / "Meshes" / "Sub");
        fs::create_directories(ci_mods / "mod_lower" / "meshes" / "sub");
        touch(ci_mods / "mod_upper" / "Meshes" / "a.nif");
        touch(ci_mods / "mod_lower" / "meshes" / "a.nif");
        touch(ci_mods / "mod_upper" / "Meshes" / "Sub" / "b.nif");
        touch(ci_mods / "mod_lower" / "meshes" / "sub" / "b.nif");

        // The two spellings must be genuinely distinct directories; on a
        // case-insensitive filesystem they collide and this test can't run
        // (mod_upper/meshes/a.nif would resolve to Meshes/a.nif).
        if (!fs::exists(ci_mods / "mod_upper" / "meshes" / "a.nif")) {
            std::vector<ConflictEngine::ModInfo> ci_list = {
                {"mod_upper", 1}, {"mod_lower", 2}};
            ConflictEngine ci_engine;
            auto ci_results = ci_engine.compute(ci_mods, ci_list, "", "", false);
            check(ci_results["mod_upper"].total_files == 2,
                  "ci: upper-cased mod indexes both files");
            check(ci_results["mod_lower"].total_files == 2,
                  "ci: lower-cased mod indexes both files");
            const auto& ci_reg = ci_engine.last_registry();
            auto k1 = ci_reg.find("meshes/a.nif");
            check(k1 != ci_reg.end() && k1->second.size() == 2,
                  "ci: Meshes/a.nif and meshes/a.nif merge into one key");
            auto k2 = ci_reg.find("meshes/sub/b.nif");
            check(k2 != ci_reg.end() && k2->second.size() == 2,
                  "ci: nested Meshes/Sub and meshes/sub merge too");
            check(ci_results["mod_lower"].wins == 2,
                  "ci: higher-priority mod wins both merged files");
            check(ci_results["mod_upper"].losses == 2,
                  "ci: lower-priority mod loses both merged files");
            std::printf("  dual-case dirs register as one CI-normalized key\n");
        } else {
            std::printf("  (skipped CI key check: case-insensitive filesystem)\n");
        }
        fs::remove_all(ci_mods);
    }

    fs::remove_all(base);
}
