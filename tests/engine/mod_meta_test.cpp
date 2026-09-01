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

TEST_CASE("mod_meta_multiline_values", "[engine]") {
  using engine::ModMeta;

  // Regression for the Nexus description mangling bug
  // (Workspace-a51p): the INI writer used to write multi-line BBCode values
  // verbatim, injecting literal newlines that broke INI structure on
  // re-parse - the value came back truncated to the first line and the rest
  // leaked into other sections. Round-trip must preserve every byte of a
  // multi-line raw BBCode payload exactly.

  // Realistic raw Nexus / Steam BBCode: paragraphs, lists, URLs, images.
  const std::string raw_bbc =
      "[url=https://ibb.co/zhWRV6F8][/url]\n"
      "[center][url=https://ibb.co/zhWRV6F8]"
      "[img]https://i.ibb.co/3y5NmRvf/photo-collage-png.png[/img][/url][/center]\n"
      "[list]\n"
      "[*] First feature\n"
      "[*] Second feature\n"
      "[*] Third feature\n"
      "[/list]\n"
      "[b]Heading[/b] trailing text on a new line.";

  // --- In-memory serialize/parse round trip. ---
  {
    ModMeta meta;
    meta.set("Nexusmods", "nexusdescription", raw_bbc);
    const std::string serialized = meta.serialize();

    // The serialized form must be a single line per key/value pair - no
    // raw newlines mid-value would mean subsequent sections get
    // mis-parsed as new section headers. We assert this directly: the
    // 'nexusdescription' line itself must be one physical line.
    bool found_desc_line = false;
    bool desc_line_is_one_physical_line = false;
    {
      // Find the start of the nexusdescription line, then walk forward
      // until the next '\n' (or end-of-string). The substring between
      // those two points must not itself contain any '\n'.
      const std::string key_prefix = "nexusdescription = ";
      size_t start = serialized.find(key_prefix);
      require(start != std::string::npos,
              "serialized form contains the nexusdescription key");
      found_desc_line = true;
      size_t end = serialized.find('\n', start);
      if (end == std::string::npos) end = serialized.size();
      desc_line_is_one_physical_line =
          serialized.find('\n', start + key_prefix.size()) >= end;
    }
    require(found_desc_line && desc_line_is_one_physical_line,
            "nexusdescription is a single physical line in the file "
            "(no embedded raw newlines)");

    // Round-trip restores the full multi-line payload.
    ModMeta parsed;
    require(parsed.parse(serialized),
            "multi-line BBCode description serializes/parses cleanly");
    require(parsed.get("Nexusmods", "nexusdescription") == raw_bbc,
            "full multi-line BBCode survives serialize/parse round trip");
  }

  // --- Sidecar file save/load round trip (the real bug repro). ---
  {
    const fs::path root = "/tmp/gmm_mod_meta_multiline/meta";
    fs::remove_all(root.parent_path());
    fs::create_directories(root);

    ModMeta meta;
    meta.set("General", "name", "Oathvein UI");
    meta.set("Nexusmods", "nexusdescription", raw_bbc);
    require(meta.save(root, "MultilineMod"), "sidecar save succeeds");

    // The on-disk file must contain the escape sequences, NOT literal
    // newlines inside the value. A literal '\n' after '[url=...]' would
    // turn the next line into a [section] header, corrupting the file.
    std::ifstream f(root / "MultilineMod.ini");
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string on_disk = ss.str();
    require(on_disk.find("\n[center]") == std::string::npos,
            "no literal newline before [center] (would re-parse as section)");
    require(on_disk.find("\\n") != std::string::npos,
            "stored value contains the '\\n' escape sequence");

    // And loading the sidecar gives back the full original value.
    auto loaded = ModMeta::load(root, "MultilineMod");
    require(loaded.get("General", "name") == "Oathvein UI",
            "name survives the round trip");
    require(loaded.get("Nexusmods", "nexusdescription") == raw_bbc,
            "multi-line BBCode survives sidecar save/load round trip");
  }

  // --- A literal backslash in the value must not be lost. ---
  {
    ModMeta meta;
    meta.set("General", "path", "C:\\Users\\Mod\\nexusdescription");
    ModMeta parsed;
    require(parsed.parse(meta.serialize()),
            "backslash-containing value parses");
    require(parsed.get("General", "path") ==
                "C:\\Users\\Mod\\nexusdescription",
            "literal backslashes survive the escape/unescape round trip");
  }

  // --- A value with no control characters is unchanged. ---
  {
    ModMeta meta;
    meta.set("General", "version", "1.2.3");
    const std::string s = meta.serialize();
    require(s.find("version = 1.2.3\n") != std::string::npos,
            "plain values serialize verbatim (no spurious escapes)");
  }

  // --- \r and \r\n both round-trip. ---
  {
    ModMeta meta;
    meta.set("General", "crlf", "line1\r\nline2\nline3");
    ModMeta parsed;
    require(parsed.parse(meta.serialize()), "CRLF value parses");
    require(parsed.get("General", "crlf") == "line1\r\nline2\nline3",
            "CR and CRLF survive the round trip");
  }
}

// Namespace-isolation regression for write_game_metadata (Workspace-rvld).
//
// The MO2-style meta.ini [General] modid= field is the Nexus mod id
// namespace specifically. write_game_metadata used to accept a generic
// "modid" parameter and write it verbatim, so a LoversLab file id
// (e.g. 12345) or a Steam workshop id (e.g. 6789) ended up in
// mods/{folder}/meta.ini as "modid=12345" / "modid=6789", which MO2 and
// any reader interprets as a Nexus mod id. The fix renames the
// parameter to nexus_mod_id and requires callers to pass an empty
// string for every non-Nexus source. Empty -> "0" in the file, MO2's
// "no Nexus id" sentinel.
TEST_CASE("mod_meta_write_game_metadata_namespace_isolation", "[engine]") {
  using engine::ModMeta;

  // --- Helper: read the [General] modid= value out of a written meta.ini. ---
  auto read_modid = [](const fs::path& mod_dir) -> std::string {
    std::ifstream f(mod_dir / "meta.ini");
    std::string line;
    while (std::getline(f, line)) {
      if (line.rfind("modid=", 0) == 0) return line.substr(6);
    }
    return {};
  };

  const fs::path root = "/tmp/gmm_mod_meta_namespace_isolation";
  fs::remove_all(root);

  // --- Nexus source: id IS the Nexus mod id, written as modid=<id>. ---
  {
    const auto mod_dir = root / "NexusMod";
    fs::create_directories(mod_dir);
    require(ModMeta::write_game_metadata(mod_dir, "meta.ini",
                                          "Some Nexus Mod", "1.0",
                                          "12345"),
            "Nexus: write_game_metadata succeeds");
    require(read_modid(mod_dir) == "12345",
            "Nexus: meta.ini modid= is the actual Nexus mod id");
  }

  // --- LoversLab: source id is a LoversLab file id; meta.ini modid
  //     must NOT carry it (would fake a Nexus attribution). ---
  {
    const auto mod_dir = root / "LoversLabMod";
    fs::create_directories(mod_dir);
    // Caller passes empty (the install path now does this for any
    // non-nexus source).
    require(ModMeta::write_game_metadata(mod_dir, "meta.ini",
                                          "LoversLab Mod", "1.0",
                                          ""),
            "LoversLab: write_game_metadata succeeds");
    require(read_modid(mod_dir) == "0",
            "LoversLab: meta.ini modid=0 (no Nexus attribution)");
  }

  // --- Steam: source id is a Steam workshop id; meta.ini modid must
  //     NOT carry it. ---
  {
    const auto mod_dir = root / "SteamMod";
    fs::create_directories(mod_dir);
    require(ModMeta::write_game_metadata(mod_dir, "meta.ini",
                                          "Steam Workshop Mod", "1.0",
                                          ""),
            "Steam: write_game_metadata succeeds");
    require(read_modid(mod_dir) == "0",
            "Steam: meta.ini modid=0 (no Nexus attribution)");
  }

  // --- Manual: no source at all; meta.ini modid must be 0. ---
  {
    const auto mod_dir = root / "ManualMod";
    fs::create_directories(mod_dir);
    require(ModMeta::write_game_metadata(mod_dir, "meta.ini",
                                          "Manual Mod", "1.0",
                                          ""),
            "Manual: write_game_metadata succeeds");
    require(read_modid(mod_dir) == "0",
            "Manual: meta.ini modid=0 (no Nexus attribution)");
  }

  // --- The full file content for a LoversLab/Steam/Manual install has
  //     the [General] block and no Nexus-like modid. ---
  {
    const auto mod_dir = root / "FullFileMod";
    fs::create_directories(mod_dir);
    require(ModMeta::write_game_metadata(mod_dir, "meta.ini",
                                          "Full File Mod", "1.0",
                                          ""),
            "FullFile: write_game_metadata succeeds");
    std::ifstream f(mod_dir / "meta.ini");
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string content = ss.str();
    require(content.find("[General]\n") != std::string::npos,
            "non-Nexus install: meta.ini has [General] section");
    require(content.find("modid=0\n") != std::string::npos,
            "non-Nexus install: meta.ini has explicit modid=0 sentinel");
    require(content.find("version=1.0\n") != std::string::npos,
            "non-Nexus install: meta.ini has version line");
  }

  // --- Empty-moddir / empty-metadata (the create_empty_mod path). ---
  {
    const auto mod_dir = root / "EmptyMod";
    fs::create_directories(mod_dir);
    require(ModMeta::write_game_metadata(mod_dir, "meta.ini",
                                          "Empty", "1.0",
                                          ""),
            "Empty: write_game_metadata succeeds for create_empty_mod");
    require(read_modid(mod_dir) == "0",
            "Empty: meta.ini modid=0 (no Nexus attribution)");
  }

  // --- Overwrite -> mod: also passes empty (the promote-overwrite
  //     folder has no Nexus provenance). ---
  {
    const auto mod_dir = root / "OverwriteMod";
    fs::create_directories(mod_dir);
    require(ModMeta::write_game_metadata(mod_dir, "meta.ini",
                                          "OverwriteMod", "1.0",
                                          ""),
            "Overwrite: write_game_metadata succeeds");
    require(read_modid(mod_dir) == "0",
            "Overwrite: meta.ini modid=0 (no Nexus attribution)");
  }

  // --- Idempotence: re-calling on an existing meta.ini is a no-op
  //     (returns true, does not rewrite). The Nexus id stays put. ---
  {
    const auto mod_dir = root / "NexusMod";
    require(ModMeta::write_game_metadata(mod_dir, "meta.ini",
                                          "Different Name", "9.9",
                                          "99999"),
            "Nexus: re-write on existing meta.ini is a no-op success");
    require(read_modid(mod_dir) == "12345",
            "Nexus: re-write does not change an existing modid");
  }
}