#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/vfs/path_resolver.h"

namespace fs = std::filesystem;

namespace {

// Build a small on-disk tree with deliberately MIXED casing so we can prove the
// resolver matches case-insensitively on a case-sensitive (Linux) filesystem.
struct TempTree {
  fs::path root;
  TempTree() {
    root = fs::temp_directory_path() / "gmm_vfs_test";
    fs::remove_all(root);
    fs::create_directories(root / "Data" / "Meshes");
    fs::create_directories(root / "Data" / "Textures");
    fs::create_directories(root / "Docs");
    write(root / "Data" / "Meshes" / "Weird.NIF");
    write(root / "Data" / "Textures" / "Bar.DDS");
    write(root / "Docs" / "ReadMe.TXT");
    write(root / "RootFile.ESP");
  }
  ~TempTree() { fs::remove_all(root); }
  static void write(const fs::path &p) {
    std::ofstream(p, std::ios::binary).put('x');
  }
};

} // namespace

TEST_CASE("PathResolver resolves case-insensitively", "[engine][vfs]") {
  TempTree t;
  engine::vfs::PathResolver r(t.root,
                              engine::vfs::NameCompare::CaseInsensitive);

  SECTION("resolve finds on-disk casing via CI input") {
    const auto gf = r.resolve("data/meshes/weird.nif");
    REQUIRE(gf.has_value());
    REQUIRE(gf->absolute() == t.root / "Data" / "Meshes" / "Weird.NIF");
    // logical() preserves the caller's original spelling.
    REQUIRE(gf->logical() == "data/meshes/weird.nif");
    REQUIRE(gf->exists());
  }

  SECTION("exists() agrees with resolve()") {
    REQUIRE(r.exists("DATA/TEXTURES/BAR.dds"));
    REQUIRE(r.exists("rootfile.esp"));
    REQUIRE_FALSE(r.exists("data/meshes/missing.nif"));
    REQUIRE_FALSE(r.exists(""));              // empty rejected
    REQUIRE_FALSE(r.exists("../etc/passwd")); // traversal rejected
    REQUIRE_FALSE(r.exists("/abs/path"));     // absolute rejected
  }

  SECTION("normalize() is the FULL CI key (filename lowered too)") {
    REQUIRE(r.normalize("Data/Meshes/Weird.NIF") == "data/meshes/weird.nif");
    REQUIRE(r.normalize("ROOTFILE.ESP") == "rootfile.esp");
  }

  SECTION("list() returns real-cased entries under a CI directory key") {
    const auto entries = r.list("data");
    REQUIRE_FALSE(entries.empty());
    bool saw_meshes = false, saw_textures = false;
    for (const auto &e : entries) {
      const auto name = e.absolute().filename().string();
      if (name == "Meshes")
        saw_meshes = true;
      if (name == "Textures")
        saw_textures = true;
      // normalized() of each entry is the full CI key.
      REQUIRE(e.normalized() ==
              engine::vfs::PathResolver(t.root).normalize(
                  "data/" + e.absolute().filename().string()));
    }
    REQUIRE(saw_meshes);
    REQUIRE(saw_textures);
  }

  SECTION("list(\"\") lists the root") {
    const auto entries = r.list("");
    REQUIRE_FALSE(entries.empty());
    bool saw_rootfile = false;
    for (const auto &e : entries) {
      if (e.absolute().filename().string() == "RootFile.ESP")
        saw_rootfile = true;
    }
    REQUIRE(saw_rootfile);
  }

  SECTION("invalidate drops cached knowledge") {
    // Warm the cache, then confirm a fresh resolve still works; invalidating
    // the root must not break subsequent resolves (cache just rebuilds).
    REQUIRE(r.resolve("data/meshes/weird.nif").has_value());
    r.invalidate_all();
    REQUIRE(r.resolve("data/meshes/weird.nif").has_value());
    r.invalidate("data/meshes/weird.nif");
    REQUIRE(r.resolve("data/meshes/weird.nif").has_value());
  }

  SECTION("introspection") {
    REQUIRE(r.root() == t.root);
    REQUIRE_FALSE(r.is_native_ci()); // Linux: indexed backend, not native CI
  }
}

TEST_CASE("PathResolver CaseSensitive mode", "[engine][vfs]") {
  TempTree t;
  engine::vfs::PathResolver r(t.root, engine::vfs::NameCompare::CaseSensitive);

  SECTION("exact casing required") {
    REQUIRE(r.resolve("Data/Meshes/Weird.NIF").has_value());
    REQUIRE_FALSE(r.resolve("data/meshes/weird.nif").has_value());
    REQUIRE(r.normalize("Data/Meshes/Weird.NIF") == "Data/Meshes/Weird.NIF");
  }
}
