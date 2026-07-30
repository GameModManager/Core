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

    fs::remove_all(base);

    std::printf("PASS: conflict_test\n");
    return 0;
}
