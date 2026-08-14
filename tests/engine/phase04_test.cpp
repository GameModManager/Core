#include "engine/index/conflict_index.h"
#include "engine/mod/model/profile.h"
#include "engine/deploy/order_hook.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

TEST_CASE("phase04", "[engine]") {
    using namespace engine;

    // --- ConflictIndex test ---
    ConflictIndex index;

    // Two mods both provide "meshes/rock.nif"
    index.add_file("meshes/rock.nif", "modA", 0);
    index.add_file("meshes/rock.nif", "modB", 1);

    // modB wins (higher priority = later in list)
    REQUIRE(index.winner("meshes/rock.nif") == "modB");

    // modA also provides "textures/grass.dds" exclusively
    index.add_file("textures/grass.dds", "modA", 0);
    REQUIRE(index.winner("textures/grass.dds") == "modA");

    // Remove modA — modB should win meshes, nothing should win textures
    index.remove_mod("modA");
    REQUIRE(index.winner("meshes/rock.nif") == "modB");
    REQUIRE(index.winner("textures/grass.dds").empty());

    // Re-add modA at higher priority
    index.add_file("meshes/rock.nif", "modA", 2);
    REQUIRE(index.winner("meshes/rock.nif") == "modA");

    std::printf("PASS: conflict_index_test\n");

    // --- Profile test ---
    Profile profile;
    profile.add_mod("modA");
    profile.add_mod("modB");
    profile.add_mod("modC");

    REQUIRE(profile.priority_of("modA") == 0);
    REQUIRE(profile.priority_of("modB") == 1);
    REQUIRE(profile.priority_of("modC") == 2);

    // Move modC to first position (highest priority)
    profile.move_mod("modC", 0);
    REQUIRE(profile.priority_of("modC") == 0);
    REQUIRE(profile.priority_of("modA") == 1);

    // Disable modB
    profile.set_enabled("modB", false);
    auto enabled = profile.enabled_in_order();
    REQUIRE(enabled.size() == 3);  // modC, modA, __overwrite__ (auto-pinned)
    REQUIRE(enabled[0] == "modC");
    REQUIRE(enabled[1] == "modA");
    REQUIRE(enabled[2] == "__overwrite__");

    std::printf("PASS: profile_test\n");

    // --- OrderEncodingHook test ---
    SkyrimPluginsTxtHook hook;
    auto output_dir = fs::temp_directory_path() / "gmm_test_order";
    fs::create_directories(output_dir);
    auto output_path = output_dir / "plugins.txt";

    std::vector<std::string> order = {"modC", "modA", "modB"};
    REQUIRE(hook.write_order(order, output_path));

    std::ifstream in(output_path);
    std::string line;
    std::getline(in, line);
    REQUIRE(line == "modC");
    std::getline(in, line);
    REQUIRE(line == "modA");
    std::getline(in, line);
    REQUIRE(line == "modB");

    fs::remove_all(output_dir);
}
