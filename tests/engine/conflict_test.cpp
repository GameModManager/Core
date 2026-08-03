#include "engine/index/conflict_engine.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>

int main() {
    using namespace engine;
    namespace fs = std::filesystem;

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

    {
        auto results = engine.compute(base, mods, "", "metadata.xml", true);

        assert(results.size() == 3);
        assert(results["mod_a"].total_files == 2);
        assert(results["mod_a"].wins == 1);
        assert(results["mod_a"].losses == 0);
        assert(results["mod_b"].total_files == 2);
        assert(results["mod_b"].wins == 0);
        assert(results["mod_b"].losses == 1);
        assert(results["mod_c"].total_files == 2);
        assert(results["mod_c"].wins == 0);
        assert(results["mod_c"].losses == 1);

        std::printf("  conflict_reversed=true: mod_a (pri=1) wins config.ini\n");
    }

    {
        auto results = engine.compute(base, mods, "", "metadata.xml", false);

        assert(results.size() == 3);
        assert(results["mod_c"].total_files == 2);
        assert(results["mod_c"].wins == 1);
        assert(results["mod_c"].losses == 0);
        assert(results["mod_b"].total_files == 2);
        assert(results["mod_b"].wins == 0);
        assert(results["mod_b"].losses == 1);
        assert(results["mod_a"].total_files == 2);
        assert(results["mod_a"].wins == 0);
        assert(results["mod_a"].losses == 1);

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
        assert(it != reg.end() && it->second.size() == 2 &&
               "mod_c's hidden copy must not stay among config.ini's owners");
        assert(reg.find("data/config.ini.gmmhidden") != reg.end() &&
               "the renamed hidden file must appear as its own registry key");

        // Restore for the directory cleanup below.
        fs::rename(base / "mod_c" / "data" / "config.ini.gmmhidden",
                   base / "mod_c" / "data" / "config.ini");
        std::printf("  invalidate_mod after a subdir rename re-scans the mod\n");
    }

    fs::remove_all(base);

    std::printf("PASS: conflict_test\n");
    return 0;
}
