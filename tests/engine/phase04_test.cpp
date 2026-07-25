#include "engine/index/conflict_index.h"
#include "engine/model/profile.h"
#include "engine/deploy/order_hook.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

int main() {
    using namespace engine;

    // --- ConflictIndex test ---
    ConflictIndex index;

    // Two mods both provide "meshes/rock.nif"
    index.add_file("meshes/rock.nif", "modA", 0);
    index.add_file("meshes/rock.nif", "modB", 1);

    // modB wins (higher priority = later in list)
    assert(index.winner("meshes/rock.nif") == "modB");

    // modA also provides "textures/grass.dds" exclusively
    index.add_file("textures/grass.dds", "modA", 0);
    assert(index.winner("textures/grass.dds") == "modA");

    // Remove modA — modB should win meshes, nothing should win textures
    index.remove_mod("modA");
    assert(index.winner("meshes/rock.nif") == "modB");
    assert(index.winner("textures/grass.dds").empty());

    // Re-add modA at higher priority
    index.add_file("meshes/rock.nif", "modA", 2);
    assert(index.winner("meshes/rock.nif") == "modA");

    std::printf("PASS: conflict_index_test\n");

    // --- Profile test ---
    Profile profile;
    profile.add_mod("modA");
    profile.add_mod("modB");
    profile.add_mod("modC");

    assert(profile.priority_of("modA") == 0);
    assert(profile.priority_of("modB") == 1);
    assert(profile.priority_of("modC") == 2);

    // Move modC to first position (highest priority)
    profile.move_mod("modC", 0);
    assert(profile.priority_of("modC") == 0);
    assert(profile.priority_of("modA") == 1);

    // Disable modB
    profile.set_enabled("modB", false);
    auto enabled = profile.enabled_in_order();
    assert(enabled.size() == 2);
    assert(enabled[0] == "modC");
    assert(enabled[1] == "modA");

    std::printf("PASS: profile_test\n");

    // --- OrderEncodingHook test ---
    SkyrimPluginsTxtHook hook;
    auto output_dir = fs::temp_directory_path() / "gmm_test_order";
    fs::create_directories(output_dir);
    auto output_path = output_dir / "plugins.txt";

    std::vector<std::string> order = {"modC", "modA", "modB"};
    assert(hook.write_order(order, output_path));

    std::ifstream in(output_path);
    std::string line;
    std::getline(in, line);
    assert(line == "modC");
    std::getline(in, line);
    assert(line == "modA");
    std::getline(in, line);
    assert(line == "modB");

    fs::remove_all(output_dir);
    std::printf("PASS: order_encoding_hook_test\n");

    std::printf("ALL PHASE 0.4 TESTS PASSED\n");
    return 0;
}
