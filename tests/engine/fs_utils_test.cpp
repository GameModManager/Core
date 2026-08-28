#include "engine/core/util/fs_utils.h"
#include "engine/core/vfs/path_resolver.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

static void touch(const fs::path& p) { std::ofstream(p).put('\n'); }

static bool path_is(const fs::path& got, const fs::path& want) {
    return got.lexically_normal() == want.lexically_normal();
}

TEST_CASE("fs utils", "[engine]") {
    using namespace engine;

    const fs::path base = fs::temp_directory_path() / "fs_utils_test_core";
    fs::remove_all(base);
    fs::create_directories(base / "Meshes" / "Armor");
    fs::create_directories(base / "Skeleton Rig" / "HDT");
    fs::create_directories(base / "Fomod");
    touch(base / "Meshes" / "Armor" / "armor.nif");
    touch(base / "Skeleton Rig" / "HDT" / "hdt.esm");
    touch(base / "Fomod" / "ModuleConfig.xml");

    // --- normalize_separators / toLower --------------------------------------
    {
        REQUIRE(normalize_separators("a\\b\\c") == "a/b/c");
        REQUIRE(normalize_separators("a/b/c") == "a/b/c");
        REQUIRE(normalize_separators("Skeleton Rig\\HDT") == "Skeleton Rig/HDT");
        REQUIRE(normalize_separators("") == "");
        REQUIRE(toLower("Fomod") == "fomod");
        REQUIRE(toLower("MODULE") == "module");
        std::printf("  normalize_separators/toLower: OK\n");
    }

    // --- name_matches_ci / resolve_regular_file_ci ---------------------------
    {
        REQUIRE(name_matches_ci(fs::path("Meshes"), "meshes"));
        REQUIRE(name_matches_ci(fs::path("meshes"), "MESHES"));
        REQUIRE(!name_matches_ci(fs::path("Meshes"), "meshesx"));
        REQUIRE(resolve_regular_file_ci(base / "Fomod", "moduleconfig.xml") ==
               base / "Fomod" / "ModuleConfig.xml");
        REQUIRE(resolve_regular_file_ci(base / "Fomod", "nope.xml").empty());
        std::printf("  name_matches_ci/resolve_regular_file_ci: OK\n");
    }

    // --- PathResolver: Windows separators + case + spaces ------------------
    {
        auto gf = vfs::PathResolver(base).resolve("Meshes\\Armor\\armor.nif");
        REQUIRE(gf.has_value());
        REQUIRE(path_is(gf->absolute(), base / "Meshes" / "Armor" / "armor.nif"));

        // Matches case-insensitively and returns the on-disk casing.
        gf = vfs::PathResolver(base).resolve("meshes\\armor\\armor.nif");
        REQUIRE(gf.has_value());
        REQUIRE(gf->absolute().filename().string() == "armor.nif");
        REQUIRE(gf->absolute().parent_path().filename().string() == "Armor");

        // Directory with spaces, resolved as a path.
        gf = vfs::PathResolver(base).resolve("Skeleton Rig\\HDT");
        REQUIRE(gf.has_value());
        REQUIRE(path_is(gf->absolute(), base / "Skeleton Rig" / "HDT"));

        // Trailing slash resolves to the directory itself.
        gf = vfs::PathResolver(base).resolve("Meshes\\Armor\\");
        REQUIRE(gf.has_value());
        REQUIRE(path_is(gf->absolute(), base / "Meshes" / "Armor"));

        std::printf("  PathResolver separators/case/spaces: OK\n");
    }

    // --- PathResolver: missing / traversal / absolute / empty --------------
    {
        REQUIRE(!vfs::PathResolver(base).resolve("meshes\\nope\\x.nif").has_value()); // absent
        REQUIRE(!vfs::PathResolver(base).resolve("..\\evil.txt").has_value());        // escape
        REQUIRE(!vfs::PathResolver(base).resolve("Meshes/../../evil.txt").has_value()); // escape
        REQUIRE(!vfs::PathResolver(base).resolve("/etc/passwd").has_value());          // absolute
        REQUIRE(!vfs::PathResolver(base).resolve("\\\\server\\share").has_value());    // UNC = absolute
        REQUIRE(!vfs::PathResolver(base).resolve("").has_value());                     // empty
        std::printf("  PathResolver missing/traversal/absolute/empty: OK\n");
    }

    // --- relay_output_to_mod: P2 route-everything -------------------------------
    // An "Output to mod" session captures game-root-relative writes into a
    // scratch dir. P2: EVERY file goes into the mod; nothing falls through to
    // Overwrite - the mod is the full write target (MO2 Custom Mods parity).
    {
        const fs::path base_r = base / "relay";
        const fs::path scratch = base_r / "scratch";
        const fs::path mod = base_r / "mod" / "MyMod";
        const fs::path ow = base_r / "overwrite";
        fs::create_directories(scratch / "Data" / "Meshes");
        fs::create_directories(scratch / "Config");
        fs::create_directories(mod);
        fs::create_directories(ow);

        // Data-relative (Skyrim) + a game-root file (also routed into the mod).
        touch(scratch / "Data" / "Meshes" / "a.nif");
        touch(scratch / "Config" / "game.ini");

        // Skyrim: mods_subpath "Data", not include_mod_id. Both files must land
        // in the mod; Overwrite must stay empty.
        auto n = relay_output_to_mod(scratch, mod, ow, "Data", false, "");
        REQUIRE(n == 2);
        REQUIRE(fs::exists(mod / "Meshes" / "a.nif"));   // Data/ stripped
        REQUIRE(!fs::exists(mod / "Data"));              // no Data wrapper
        REQUIRE(fs::exists(mod / "Config" / "game.ini")); // game-root passes through, still in the mod
        REQUIRE(fs::is_empty(ow));                        // Overwrite never touched

        // Isaac-style include_mod_id: scratch/<mods_subpath>/<mod_id>/<rest>
        // maps to mod/<rest>; a file outside the mapping still lands in the mod.
        const fs::path scratch2 = base_r / "scratch2";
        const fs::path mod2 = base_r / "mod" / "IsaacMod";
        fs::create_directories(scratch2 / "mods" / "IsaacMod" / "resources" / "gfx");
        fs::create_directories(scratch2 / "top");
        fs::create_directories(mod2);
        touch(scratch2 / "mods" / "IsaacMod" / "resources" / "gfx" / "a.png");
        touch(scratch2 / "top" / "root.txt");
        auto n2 = relay_output_to_mod(scratch2, mod2, ow, "mods", true, "IsaacMod");
        REQUIRE(n2 == 2);
        REQUIRE(fs::exists(mod2 / "resources" / "gfx" / "a.png"));
        REQUIRE(fs::exists(mod2 / "top" / "root.txt"));
        REQUIRE(fs::is_empty(ow));

        fs::remove_all(base_r);
        std::printf("  relay_output_to_mod (P2 route-everything): OK\n");
    }

    std::printf("fs_utils_test: all checks passed\n");
    fs::remove_all(base);
}
