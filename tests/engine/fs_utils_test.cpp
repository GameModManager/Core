#include "engine/fs_utils.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static void touch(const fs::path& p) { std::ofstream(p).put('\n'); }

static bool path_is(const fs::path& got, const fs::path& want) {
    return got.lexically_normal() == want.lexically_normal();
}

int main() {
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
        assert(normalize_separators("a\\b\\c") == "a/b/c");
        assert(normalize_separators("a/b/c") == "a/b/c");
        assert(normalize_separators("Skeleton Rig\\HDT") == "Skeleton Rig/HDT");
        assert(normalize_separators("") == "");
        assert(toLower("Fomod") == "fomod");
        assert(toLower("MODULE") == "module");
        std::printf("  normalize_separators/toLower: OK\n");
    }

    // --- name_matches_ci / find_file_ci ---------------------------------------
    {
        assert(name_matches_ci(fs::path("Meshes"), "meshes"));
        assert(name_matches_ci(fs::path("meshes"), "MESHES"));
        assert(!name_matches_ci(fs::path("Meshes"), "meshesx"));
        assert(find_file_ci(base / "Fomod", "moduleconfig.xml") ==
               base / "Fomod" / "ModuleConfig.xml");
        assert(find_file_ci(base / "Fomod", "nope.xml").empty());
        std::printf("  name_matches_ci/find_file_ci: OK\n");
    }

    // --- resolve_path: Windows separators + case + spaces ---------------------
    {
        bool escaped = false;
        auto p = resolve_path(base, "Meshes\\Armor\\armor.nif", &escaped);
        assert(!p.empty() && !escaped);
        assert(path_is(p, base / "Meshes" / "Armor" / "armor.nif"));

        // Matches case-insensitively and returns the on-disk casing.
        p = resolve_path(base, "meshes\\armor\\armor.nif");
        assert(!p.empty());
        assert(p.filename().string() == "armor.nif");
        assert(p.parent_path().filename().string() == "Armor");

        // Directory with spaces, resolved as a path.
        p = resolve_path(base, "Skeleton Rig\\HDT");
        assert(!p.empty());
        assert(path_is(p, base / "Skeleton Rig" / "HDT"));

        // Trailing slash resolves to the directory itself.
        p = resolve_path(base, "Meshes\\Armor\\");
        assert(!p.empty());
        assert(path_is(p, base / "Meshes" / "Armor"));

        std::printf("  resolve_path separators/case/spaces: OK\n");
    }

    // --- resolve_path: missing / traversal / absolute / empty -----------------
    {
        bool escaped = false;
        auto p = resolve_path(base, "meshes\\nope\\x.nif", &escaped);
        assert(p.empty() && !escaped);  // absent, not an escape

        escaped = false;
        p = resolve_path(base, "..\\evil.txt", &escaped);
        assert(p.empty() && escaped);

        escaped = false;
        p = resolve_path(base, "Meshes/../../evil.txt", &escaped);
        assert(p.empty() && escaped);

        escaped = false;
        p = resolve_path(base, "/etc/passwd", &escaped);
        assert(p.empty() && escaped);

        escaped = false;
        p = resolve_path(base, "\\\\server\\share", &escaped);  // UNC = absolute
        assert(p.empty() && escaped);

        escaped = false;
        p = resolve_path(base, "", &escaped);
        assert(p.empty() && escaped);

        std::printf("  resolve_path missing/traversal/absolute/empty: OK\n");
    }

    std::printf("fs_utils_test: all checks passed\n");
    fs::remove_all(base);
    return 0;
}
