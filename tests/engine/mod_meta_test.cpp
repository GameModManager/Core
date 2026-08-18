// Engine test for the manager-sidecar per-mod UI state keys added for
// Issue #35: [GameModManager] folded (tree-view collapse) and parent_id
// (visual-nesting link; absent = top-level). These live in the manager
// sidecar ({instance_root}/meta/{folder_name}.ini), NOT the mod's own
// MO2-format meta.ini.
//
// Covers: folded true/false round-trip through serialize/parse, parent_id
// set/get, unset() removing a key back to "absent", and the sidecar
// file save/load round trip.
#include "engine/mod/meta/mod_meta.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {
void require(bool cond, const char *msg) {
  INFO(msg);
  REQUIRE(cond);
}
} // namespace

TEST_CASE("mod_meta_ui_state", "[engine]") {
  using engine::ModMeta;

  // --- folded round-trip through serialize/parse. ---
  {
    ModMeta meta;
    require(!meta.folded(), "fresh meta is unfolded");
    meta.set_folded(true);
    require(meta.folded(), "set_folded(true) reads back true");
    ModMeta parsed;
    require(parsed.parse(meta.serialize()), "serialized folded=true parses");
    require(parsed.folded(), "folded=true survives serialize/parse");
    require(parsed.get("GameModManager", "folded") == "true",
            "folded key is explicit true");

    parsed.set_folded(false);
    require(!parsed.folded(), "set_folded(false) reads back false");
    ModMeta parsed2;
    require(parsed2.parse(parsed.serialize()),
            "serialized folded=false parses");
    require(!parsed2.folded(), "folded=false survives serialize/parse");
    require(parsed2.get("GameModManager", "folded") == "false",
            "folded key is explicit false");
  }

  // --- parent_id set/get + unset back to absent. ---
  {
    ModMeta meta;
    require(meta.parent_id().empty(), "fresh meta has no parent (top-level)");
    meta.set_parent_id("ParentMod");
    require(meta.parent_id() == "ParentMod", "set_parent_id reads back");
    ModMeta parsed;
    require(parsed.parse(meta.serialize()), "serialized parent_id parses");
    require(parsed.parent_id() == "ParentMod",
            "parent_id survives serialize/parse");

    // unset() removes the key entirely: absent = top-level.
    parsed.unset("GameModManager", "parent_id");
    require(parsed.parent_id().empty(), "unset clears parent_id");
    bool has_key = false;
    for (const auto &k : parsed.keys("GameModManager"))
      if (k == "parent_id")
        has_key = true;
    require(!has_key, "unset removes the key from the section");
    ModMeta parsed2;
    require(parsed2.parse(parsed.serialize()),
            "serialized meta without parent_id parses");
    require(parsed2.parent_id().empty(),
            "absent parent_id reads back as top-level");
  }

  // --- Sidecar file save/load round trip. ---
  {
    const fs::path root = "/tmp/gmm_mod_meta_ui_state/meta";
    fs::remove_all(root.parent_path());
    fs::create_directories(root);

    ModMeta meta;
    meta.set_folded(true);
    meta.set_parent_id("ParentMod");
    require(meta.save(root, "ChildMod"), "sidecar save succeeds");

    auto loaded = ModMeta::load(root, "ChildMod");
    require(loaded.folded(), "sidecar load restores folded");
    require(loaded.parent_id() == "ParentMod",
            "sidecar load restores parent_id");

    // A mod without a sidecar file loads as empty (no fold, no parent).
    auto missing = ModMeta::load(root, "NoSuchMod");
    require(!missing.folded() && missing.parent_id().empty(),
            "missing sidecar loads as unfolded top-level");
  }
}

TEST_CASE("mod_meta_category_csv", "[engine]") {
  using engine::ModMeta;

  // --- [General] "category" CSV (primary first) round-trip. ---
  {
    ModMeta meta;
    meta.set("General", "category", "9,24,14");
    ModMeta parsed;
    require(parsed.parse(meta.serialize()), "serialized category CSV parses");
    require(parsed.get("General", "category") == "9,24,14",
            "category CSV survives serialize/parse");
  }

  // --- Sidecar file save/load round trip (the context-menu write path). ---
  {
    const fs::path root = "/tmp/gmm_mod_meta_category/meta";
    fs::remove_all(root.parent_path());
    fs::create_directories(root);

    ModMeta meta;
    meta.set("General", "category", "9,24,14");
    require(meta.save(root, "ModWithCategories"), "sidecar save succeeds");

    auto loaded = ModMeta::load(root, "ModWithCategories");
    require(loaded.get("General", "category") == "9,24,14",
            "sidecar load restores the category CSV");

    // A mod without categories has no [General] category key.
    auto missing = ModMeta::load(root, "NoSuchMod");
    require(missing.get("General", "category").empty(),
            "missing sidecar has no category CSV");
  }
}