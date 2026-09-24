// Engine test for the save-parser infrastructure Core still owns
// (Workspace-xezt: Gamebryo parsing moved into the Plugins packet).
//
// What this file tests:
//   - SaveParserRegistry: register / has_parser / parse_save dispatch / priority
//     resolution (highest wins) / clear_plugin drop / missing-fn no-op /
//     parse_save on a game_id with no parser returns nullopt.
//   - scan_saves: filters by extension, swallows SaveParseError, sorts by
//     creation_time newest first, never crashes on an empty parse_fn.
//   - SavesScanWorker-style fallback: when has_parser(game_id) is false, the
//     stub returns SaveGame{file_path, game_id, mtime} (Workspace-c48h path)
//     so the Saves tab still lists files for an unknown game.
//   - End-to-end: register a stub parser, scan via the registry lambda, verify
//     the parser's metadata lands in the result.
//   - find_save_missing_assets: the original MO2-port coverage kept verbatim
//     (it never depended on the moved parsers).
//
// What this file does NOT test anymore (it lives in the Plugins packet now):
//   - TESV_SAVEGAME magic check, header layout, RGB vs RGBA screenshot,
//     plugin-list encoding, compression types 0/1/2, light_plugins.

#include "engine/game/registry/game_knowledge.h"
#include "engine/game/saves/save_fast_scan.h"
#include "engine/game/saves/save_game.h"
#include "engine/game/saves/save_missing_assets.h"
#include "engine/game/saves/save_reader.h"
#include "engine/game/saves/save_scanner.h"
#include "engine/pipeline/plugin_host/save_parser_registry.h"

#include <zlib.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace engine;

namespace fs = std::filesystem;

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

void write_file(const fs::path &p, const std::string &data) {
  std::ofstream(p, std::ios::binary)
      .write(data.data(), static_cast<std::streamsize>(data.size()));
}

// Touch a file with a given mtime (used to give two saves a known
// newest-first ordering without depending on real SE file format).
void touch_with_mtime(const fs::path &p, std::chrono::seconds offset) {
  std::ofstream(p, std::ios::binary).put('x');
  auto base = std::chrono::file_clock::now();
  // Pull offset back from "now" so older writes land earlier.
  auto t = base - std::chrono::duration_cast<std::chrono::file_clock::duration>(offset);
  std::error_code ec;
  std::filesystem::last_write_time(p, t, ec);
  (void)ec;
}

struct ScopedClear {
  ~ScopedClear() { SaveParserRegistry::instance().clear(); }
};
}  // namespace

// --- SaveParserRegistry: dispatch + priority + clear ---
TEST_CASE("save parser registry: dispatch + priority + clear", "[engine]") {
  ScopedClear clear;
  auto &reg = SaveParserRegistry::instance();

  check(!reg.has_parser("stubgame"), "no parser registered initially");

  int a_calls = 0;
  int b_calls = 0;
  reg.register_parser(
      "stubgame", 10,
      [&a_calls](const fs::path &, const std::string &gid) {
        ++a_calls;
        SaveGame g;
        g.game_id = gid;
        g.pc_name = "from-A";
        return g;
      },
      nullptr, "pluginA");

  reg.register_parser(
      "stubgame", 100,
      [&b_calls](const fs::path &, const std::string &gid) {
        ++b_calls;
        SaveGame g;
        g.game_id = gid;
        g.pc_name = "from-B";
        return g;
      },
      nullptr, "pluginB");

  check(reg.has_parser("stubgame"), "has_parser after register");
  auto resolved = reg.parse_save(fs::path("/nope.ess"), "stubgame");
  check(resolved.has_value(), "parse_save returns the highest-priority parser");
  check(resolved->pc_name == "from-B", "B wins (priority 100 > 10)");
  check(a_calls == 0, "A never called when shadowed");
  check(b_calls == 1, "B invoked exactly once");

  // Drop A; B remains.
  reg.clear_plugin("pluginA");
  auto after_clear = reg.parse_save(fs::path("/nope.ess"), "stubgame");
  check(after_clear.has_value(), "B still resolves after A cleared");
  check(after_clear->pc_name == "from-B", "B still wins");

  // Drop B; parser now absent.
  reg.clear_plugin("pluginB");
  check(!reg.has_parser("stubgame"), "no parser after both cleared");
  auto empty = reg.parse_save(fs::path("/nope.ess"), "stubgame");
  check(!empty.has_value(), "parse_save returns nullopt when nothing registered");
}

// --- SaveParserRegistry: null fn + cross-game_id isolation ---
TEST_CASE("save parser registry: null fn and cross-game isolation", "[engine]") {
  ScopedClear clear;
  auto &reg = SaveParserRegistry::instance();

  reg.register_parser("g1", 50, nullptr, nullptr, "pluginX");
  check(!reg.has_parser("g1"), "null fn is ignored, no entry added");

  reg.register_parser(
      "g1", 50,
      [](const fs::path &, const std::string &) {
        return SaveGame{};
      },
      nullptr, "pluginX");
  check(reg.has_parser("g1"), "real fn registers");
  check(!reg.has_parser("g2"), "g2 has no parser");
  auto g2 = reg.parse_save(fs::path("/nope"), "g2");
  check(!g2.has_value(), "parse_save for unrelated game_id returns nullopt");
}

// --- scan_saves: extension filter, SaveParseError swallow, sort, empty parse_fn ---
TEST_CASE("scan_saves: extension filter + sort + error swallow", "[engine]") {
  const fs::path root = fs::temp_directory_path() / "gmm_scan_saves_test";
  fs::remove_all(root);
  fs::create_directories(root);

  // Three .ess, one .skse (co-save, must be filtered out), one .dat.
  touch_with_mtime(root / "old.ess", std::chrono::seconds(60));
  touch_with_mtime(root / "new.ess", std::chrono::seconds(10));
  touch_with_mtime(root / "middle.ess", std::chrono::seconds(30));
  write_file(root / "co.skse", "x");
  write_file(root / "other.dat", "x");

  // Stub parse: creation_time derived from mtime, throws on the magic
  // marker "BAD" to exercise the swallow.
  int call_count       = 0;
  SaveParseFn parse_fn = [&call_count](const fs::path &p) {
    ++call_count;
    if (p.filename() == "bad.ess") {
      throw SaveParseError("synthetic");
    }
    SaveGame g;
    g.file_path = p;
    g.game_id   = "stubgame";
    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(p, ec);
    if (!ec) {
      g.creation_time = static_cast<SaveEpochSeconds>(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::clock_cast<std::chrono::system_clock>(mtime)
                  .time_since_epoch())
              .count());
    }
    return g;
  };

  write_file(root / "bad.ess", "BAD");  // this one must be skipped

  auto saves = scan_saves(root, {"ess", ".ess"}, parse_fn);
  check(saves.size() == 3, "three .ess survive extension + error filter");
  check(call_count == 4, "parse_fn called for 4 .ess files; one threw, was swallowed");
  for (std::size_t i = 1; i < saves.size(); ++i) {
    check(saves[i - 1].creation_time >= saves[i].creation_time, "newest first");
  }
  check(saves.front().file_path.filename() == "new.ess", "newest file lands first");

  // Empty parse_fn: scanner must not crash (continues silently).
  SaveParseFn empty_fn;
  auto noop = scan_saves(root, {"ess"}, empty_fn);
  check(noop.empty(), "empty parse_fn → no saves returned (silently skipped)");

  fs::remove_all(root);
}

// --- SavesScanWorker-style fallback: no parser registered, files still listed ---
TEST_CASE("save parser fallback: stub lists files when no parser registered",
          "[engine]") {
  ScopedClear clear;
  // Confirm precondition: nobody registered "noparsergame".
  check(!SaveParserRegistry::instance().has_parser("noparsergame"),
        "no parser registered for noparsergame");

  const fs::path root = fs::temp_directory_path() / "gmm_fallback_scan";
  fs::remove_all(root);
  fs::create_directories(root);
  touch_with_mtime(root / "first.ess", std::chrono::seconds(40));
  touch_with_mtime(root / "second.ess", std::chrono::seconds(20));

  // Same shape as SavesScanWorker::run in saves_scan_worker.cpp: probe the
  // registry, fall through to a stub that returns file_path + mtime.
  const std::string gid = "noparsergame";
  SaveParseFn parses    = [gid](const fs::path &p) {
    if (SaveParserRegistry::instance().has_parser(gid)) {
      auto r = SaveParserRegistry::instance().parse_save(p, gid);
      if (!r)
        throw SaveParseError("no save parser for " + gid);
      return *r;
    }
    SaveGame stub;
    stub.file_path = p;
    stub.game_id   = gid;
    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(p, ec);
    if (!ec) {
      stub.creation_time = static_cast<SaveEpochSeconds>(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::clock_cast<std::chrono::system_clock>(mtime)
                  .time_since_epoch())
              .count());
    }
    return stub;
  };

  auto saves = scan_saves(root, {"ess"}, parses);
  check(saves.size() == 2, "fallback lists both .ess files");
  check(saves[0].creation_time >= saves[1].creation_time,
        "fallback still sorts newest first");
  check(saves[0].pc_name.empty() && saves[0].plugins.empty(),
        "fallback carries no parsed metadata (only file_path + mtime)");

  fs::remove_all(root);
}

// --- End-to-end: register a stub parser, drive scan_saves through the registry ---
TEST_CASE("save parser end-to-end: registry-driven scan", "[engine]") {
  ScopedClear clear;
  auto &reg             = SaveParserRegistry::instance();
  const std::string gid = "e2egame";

  int parsed = 0;
  reg.register_parser(
      gid, 50,
      [&parsed](const fs::path &p, const std::string &g) {
        ++parsed;
        SaveGame s;
        s.file_path     = p;
        s.game_id       = g;
        s.pc_name       = "E2E";
        s.creation_time = 1234;  // constant so we can assert order
        return s;
      },
      nullptr, "e2e_plugin");

  const fs::path root = fs::temp_directory_path() / "gmm_e2e_scan";
  fs::remove_all(root);
  fs::create_directories(root);
  write_file(root / "a.ess", "anything");
  write_file(root / "b.ess", "anything");
  write_file(root / "ignore.txt", "x");

  // Mirror SavesScanWorker's lambda shape.
  SaveParseFn parses = [gid](const fs::path &p) {
    auto r = SaveParserRegistry::instance().parse_save(p, gid);
    if (!r)
      throw SaveParseError("no save parser for " + gid);
    return *r;
  };

  auto saves = scan_saves(root, {"ess"}, parses);
  check(saves.size() == 2, "registry parser sees both .ess files");
  check(parsed == 2, "parser invoked twice");
  for (const auto &s : saves) {
    check(s.game_id == gid, "game_id propagated by registry");
    check(s.pc_name == "E2E", "registry parser filled the field");
  }

  fs::remove_all(root);
}

// --- find_save_missing_assets (unchanged coverage from the previous test) ---
TEST_CASE("save missing assets", "[engine]") {
  const fs::path root = fs::temp_directory_path() / "gmm_missing_assets_test";
  fs::remove_all(root);
  fs::create_directories(root);

  // Load order with one active plugin, one inactive (present, disabled)
  // and everything else absent.
  std::vector<GamePlugin> plugins;
  {
    GamePlugin p;
    p.name      = "Skyrim.esm";
    p.enabled   = true;
    p.owner_mod = "";
    plugins.push_back(p);
  }
  {
    GamePlugin p;
    p.name      = "SkyUI_SE.esp";
    p.enabled   = false;
    p.owner_mod = "SkyUI";
    plugins.push_back(p);
  }
  {
    GamePlugin p;
    p.name      = "SomeOtherMod.esp";
    p.enabled   = true;
    p.owner_mod = "SomeOtherMod";
    plugins.push_back(p);
  }
  {
    // Force-loaded base master: disabled in the snapshot but always
    // active in-game, so never missing.
    GamePlugin p;
    p.name         = "Update.esm";
    p.enabled      = false;
    p.force_loaded = true;
    p.owner_mod    = "";
    plugins.push_back(p);
  }

  // Mods dir: "SkyUI" (disabled plugin), "GoneMod" (missing plugin),
  // "NoPlugins" (nothing relevant). Overwrite holds the missing plugin.
  const fs::path am = root / "am_mods";
  fs::create_directories(am / "SkyUI");
  fs::create_directories(am / "GoneMod");
  fs::create_directories(am / "NoPlugins");
  write_file(am / "SkyUI" / "SkyUI_SE.esp", "x");
  write_file(am / "GoneMod" / "GonePlugin.esp", "x");
  write_file(am / "NoPlugins" / "readme.txt", "x");
  const fs::path ow = root / "am_overwrite";
  fs::create_directories(ow);
  write_file(ow / "GonePlugin.esp", "x");

  SaveGame save;
  save.plugins = {"Skyrim.esm", "SkyUI_SE.esp", "GonePlugin.esp", "AlsoMissing.esm",
                  "Update.esm"};
  auto missing = find_save_missing_assets(save, plugins, am, ow);

  check(missing.size() == 3, "missing asset count");
  std::map<std::string, const SaveMissingAsset *> by_name;
  for (const auto &m : missing)
    by_name[m.plugin_name] = &m;

  auto it = by_name.find("SkyUI_SE.esp");
  check(it != by_name.end(), "inactive plugin is missing");
  check(it->second->inactive, "inactive flag set");
  check(it->second->origin_mod == "SkyUI", "inactive origin mod");
  check(std::find(it->second->providing_mods.begin(), it->second->providing_mods.end(),
                  "SkyUI") != it->second->providing_mods.end(),
        "inactive provider found");

  it = by_name.find("GonePlugin.esp");
  check(it != by_name.end(), "absent plugin is missing");
  check(!it->second->inactive, "absent plugin not flagged inactive");
  check(it->second->origin_mod.empty(), "absent plugin has no origin");
  check(std::find(it->second->providing_mods.begin(), it->second->providing_mods.end(),
                  "GoneMod") != it->second->providing_mods.end(),
        "providing mod found");
  check(std::find(it->second->providing_mods.begin(), it->second->providing_mods.end(),
                  "<overwrite>") != it->second->providing_mods.end(),
        "overwrite provider found");

  it = by_name.find("AlsoMissing.esm");
  check(it != by_name.end(), "second absent plugin is missing");
  check(it->second->providing_mods.empty(), "no provider for AlsoMissing");

  check(by_name.find("Skyrim.esm") == by_name.end(), "active plugin is not missing");

  fs::remove_all(root);
}

// --- Workspace-6kn7: build_save_provider_index + indexed find overload ---
// The worker now builds the provider index once and runs every save through
// the indexed overload, instead of re-walking the mods dir per save. Verify
// the indexed path produces the same missing-asset view as the original
// on-disk walk.
TEST_CASE("save provider index: build once, reuse for many saves", "[engine]") {
  const fs::path root = fs::temp_directory_path() / "gmm_provider_index_test";
  fs::remove_all(root);
  fs::create_directories(root);

  const fs::path am = root / "am_mods";
  const fs::path ow = root / "am_overwrite";
  fs::create_directories(am / "SkyUI");
  fs::create_directories(am / "GoneMod");
  write_file(am / "SkyUI" / "SkyUI_SE.esp", "x");
  write_file(am / "GoneMod" / "GonePlugin.esp", "x");
  fs::create_directories(ow);
  write_file(ow / "Other.esp", "x");

  const auto index = build_save_provider_index(am, ow);
  check(index.count("skyui_se.esp") == 1, "SkyUI_SE.esp indexed under lower key");
  check(index.count("goneplugin.esp") == 1, "GonePlugin.esp indexed under lower key");
  check(index.count("other.esp") == 1, "Other.esp from overwrite indexed");
  check(std::find(index.at("skyui_se.esp").begin(), index.at("skyui_se.esp").end(),
                  "SkyUI") != index.at("skyui_se.esp").end(),
        "SkyUI_SE.esp provider names SkyUI");
  check(std::find(index.at("other.esp").begin(), index.at("other.esp").end(),
                  "<overwrite>") != index.at("other.esp").end(),
        "Other.esp provider is <overwrite>");

  // The same load order and save as the on-disk-walk test above, fed
  // through the indexed overload - same missing-asset view.
  std::vector<GamePlugin> plugins;
  {
    GamePlugin p;
    p.name    = "Skyrim.esm";
    p.enabled = true;
    plugins.push_back(p);
  }
  {
    GamePlugin p;
    p.name      = "SkyUI_SE.esp";
    p.enabled   = false;
    p.owner_mod = "SkyUI";
    plugins.push_back(p);
  }
  SaveGame save;
  save.plugins = {"Skyrim.esm", "SkyUI_SE.esp", "GonePlugin.esp", "AlsoMissing.esm"};
  auto missing = find_save_missing_assets(save, plugins, index);

  std::map<std::string, const SaveMissingAsset *> by_name;
  for (const auto &m : missing)
    by_name[m.plugin_name] = &m;
  check(missing.size() == 3, "indexed overload: same 3 missing plugins");
  check(by_name.count("Skyrim.esm") == 0,
        "indexed overload: active plugin not missing");
  auto it = by_name.find("SkyUI_SE.esp");
  check(it != by_name.end() && it->second->inactive,
        "indexed overload: inactive plugin flagged");
  check(std::find(it->second->providing_mods.begin(), it->second->providing_mods.end(),
                  "SkyUI") != it->second->providing_mods.end(),
        "indexed overload: provider comes from the index");
  it = by_name.find("GonePlugin.esp");
  check(it != by_name.end() && std::find(it->second->providing_mods.begin(),
                                         it->second->providing_mods.end(),
                                         "GoneMod") != it->second->providing_mods.end(),
        "indexed overload: GonePlugin still has GoneMod provider");
  it = by_name.find("AlsoMissing.esm");
  check(it != by_name.end() && it->second->providing_mods.empty(),
        "indexed overload: truly-missing plugin has no provider");

  // Empty paths are tolerated: an instance with no mods dir or no overwrite
  // should still classify save plugins correctly, just without providers.
  const auto empty_index = build_save_provider_index({}, {});
  auto missing2          = find_save_missing_assets(save, plugins, empty_index);
  check(missing2.size() == 3, "empty index: still classifies missing plugins");
  for (const auto &m : missing2) {
    check(m.providing_mods.empty(), "empty index: no provider for any missing plugin");
  }

  fs::remove_all(root);
}

// --- Workspace-69xt: fast-scan seam + Gamebryo fast reader ---
// The Saves scan must not pay whole-file reads + full-region inflates per
// save (the 24s bug: 103 saves x full parse). These tests pin:
//   - SaveParserRegistry fast-parser dispatch/priority/clear semantics,
//   - the "save_fast_format" knowledge hook default + declared value,
//   - parse_gamebryo_tesv_fast field fidelity for SE type-0, SE type-1
//     (multi-chunk, early stop) and LE layouts,
//   - rejection (bad magic, truncated header) vs fallback (plugin data past
//     the decompressed cap asks for the full parser instead of dropping).

namespace {

void put16(std::vector<char> &v, std::uint16_t x) {
  v.push_back(static_cast<char>(x & 0xFF));
  v.push_back(static_cast<char>((x >> 8) & 0xFF));
}
void put32(std::vector<char> &v, std::uint32_t x) {
  for (int i = 0; i < 4; ++i)
    v.push_back(static_cast<char>((x >> (8 * i)) & 0xFF));
}
void put64(std::vector<char> &v, std::uint64_t x) {
  put32(v, static_cast<std::uint32_t>(x & 0xFFFFFFFFu));
  put32(v, static_cast<std::uint32_t>((x >> 32) & 0xFFFFFFFFu));
}
void putstr(std::vector<char> &v, const std::string &s) {
  put16(v, static_cast<uint16_t>(s.size()));
  v.insert(v.end(), s.begin(), s.end());
}

// Common TESV header through the FILETIME. Mirrors the Plugins packet
// fetch_information_fields order exactly.
void write_tesv_head(std::vector<char> &f, std::uint32_t version,
                     std::uint32_t save_number, const std::string &pc,
                     std::uint32_t level, const std::string &loc,
                     std::uint64_t filetime) {
  const char *magic = "TESV_SAVEGAME";
  f.insert(f.end(), magic, magic + 13);
  put32(f, 0);  // header size (unused)
  put32(f, version);
  put32(f, save_number);
  putstr(f, pc);
  put32(f, level);
  putstr(f, loc);
  putstr(f, "12:34:56");  // time of day
  putstr(f, "ImperialRace");
  put16(f, 0);  // gender
  for (int i = 0; i < 8; ++i)
    f.push_back(0);  // xp
  put64(f, filetime);
}

// SE data-region head: version/u8 + plugin + light lists (u8 counts here;
// the wide-light-count case is built by hand in its test).
void put_se_region_head(std::vector<char> &r, const std::vector<std::string> &plugins,
                        const std::vector<std::string> &light) {
  r.push_back(78);  // save game version >= 78 -> light plugins present
  r.push_back(1);   // plugin info size (unused)
  put16(r, 0);      // other (unknown)
  r.push_back(0);   // pad
  r.push_back(static_cast<char>(plugins.size()));
  for (const auto &pl : plugins)
    putstr(r, pl);
  put16(r, static_cast<std::uint16_t>(light.size()));
  for (const auto &pl : light)
    putstr(r, pl);
}

void write_file_bytes(const fs::path &p, const std::vector<char> &data) {
  std::ofstream out(p, std::ios::binary);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

// Compress `raw` in ~64KiB independent zlib streams (models the MO2 type-1
// chunk chain; stream count, not size, is what the early-stop test needs).
std::vector<std::vector<char>> zlib_streams(const std::vector<char> &raw) {
  const std::size_t kIn = 64 * 1024;
  std::vector<std::vector<char>> streams;
  for (std::size_t off = 0; off < raw.size(); off += kIn) {
    const std::size_t n = std::min(kIn, raw.size() - off);
    uLong bound         = compressBound(static_cast<uLong>(n));
    std::vector<char> out(static_cast<std::size_t>(bound));
    uLongf outlen = bound;
    INFO("zlib compress of a fixture chunk must succeed");
    REQUIRE(compress2(reinterpret_cast<Bytef *>(out.data()), &outlen,
                      reinterpret_cast<const Bytef *>(raw.data() + off),
                      static_cast<uLong>(n), Z_DEFAULT_COMPRESSION) == Z_OK);
    out.resize(static_cast<std::size_t>(outlen));
    streams.push_back(std::move(out));
  }
  return streams;
}

// Append a type-1 chunk chain for `raw` at the end of `f` (caller already
// wrote header + screenshot + compression u16).
void append_type1(std::vector<char> &f, const std::vector<char> &raw) {
  auto streams           = zlib_streams(raw);
  const std::size_t head = f.size();
  put64(f, 0);  // placeholder chunk_start
  put64(f, raw.size());
  const std::size_t chunk_start = f.size();
  for (int i = 0; i < 8; ++i)
    f[head + i] = static_cast<char>((chunk_start >> (8 * i)) & 0xFF);
  for (auto &s : streams) {
    f.insert(f.end(), s.begin(), s.end());
    while (f.size() % 16 != 0)
      f.push_back(0);  // 16-byte stream alignment (MO2 readNextChunk)
  }
}

}  // namespace

TEST_CASE("save fast format hook: declared and default", "[engine]") {
  GameKnowledge bare;
  check(save_fast_format_for(bare, "skyrimse").empty(),
        "undeclared game -> empty (scan uses the full parser)");
  GameKnowledge declared;
  declared.set("skyrimse", "save_fast_format", "gamebryo-tesv");
  check(save_fast_format_for(declared, "skyrimse") == "gamebryo-tesv",
        "declared hook value round-trips");
  check(save_fast_format_for(declared, "other").empty(),
        "hook is per-game (no cross-game leak)");
}

TEST_CASE("save parser registry: fast dispatch + priority + clear", "[engine]") {
  ScopedClear clear;
  auto &reg = SaveParserRegistry::instance();

  check(!reg.has_fast_parser("fastgame"), "no fast parser initially");
  check(!reg.has_parser("fastgame"), "fast registration never implies full");

  int lo_calls = 0;
  int hi_calls = 0;
  reg.register_fast_parser(
      "fastgame", 10,
      [&lo_calls](const fs::path &, const std::string &gid) {
        ++lo_calls;
        SaveGame g;
        g.game_id = gid;
        g.pc_name = "fast-lo";
        return g;
      },
      nullptr, "pluginLo");
  reg.register_fast_parser(
      "fastgame", 100,
      [&hi_calls](const fs::path &, const std::string &gid) {
        ++hi_calls;
        SaveGame g;
        g.game_id = gid;
        g.pc_name = "fast-hi";
        return g;
      },
      nullptr, "pluginHi");

  check(reg.has_fast_parser("fastgame"), "has_fast_parser after register");
  auto resolved = reg.parse_save_fast(fs::path("/nope.ess"), "fastgame");
  check(resolved.has_value(), "fast parse resolves the highest priority");
  check(resolved->pc_name == "fast-hi", "priority 100 wins");
  check(lo_calls == 0 && hi_calls == 1, "only the winner runs");

  reg.clear_plugin("pluginHi");
  check(reg.has_fast_parser("fastgame"), "lo survives hi's unload");
  auto after = reg.parse_save_fast(fs::path("/nope.ess"), "fastgame");
  check(after.has_value() && after->pc_name == "fast-lo", "lo now resolves");

  reg.clear_plugin("pluginLo");
  check(!reg.has_fast_parser("fastgame"), "nothing left after both unload");
  check(!reg.parse_save_fast(fs::path("/nope.ess"), "fastgame").has_value(),
        "no fast parser -> nullopt (caller falls back)");

  // Null fn is ignored, like register_parser.
  reg.register_fast_parser("fastgame", 1, nullptr, nullptr, "pluginNull");
  check(!reg.has_fast_parser("fastgame"), "null fast fn ignored");
}

TEST_CASE("gamebryo fast scan: se type-0 fidelity", "[engine]") {
  const fs::path root = fs::temp_directory_path() / "gmm_fast_se0";
  fs::remove_all(root);
  fs::create_directories(root);
  const fs::path p = root / "Q.ess";

  constexpr std::uint64_t kFt = 0x01DD2288D3CC4860ULL;
  std::vector<char> f;
  write_tesv_head(f, 12, 7, "Vanny", 40, "Whiterun", kFt);
  put32(f, 64);
  put32(f, 48);
  put16(f, 0);  // compression: raw
  for (int i = 0; i < 64 * 48 * 4; ++i)
    f.push_back(static_cast<char>(i & 0xFF));  // RGBA shot (skipped)
  // Raw region head: must be identical to what the full parser reads.
  f.push_back(78);
  f.push_back(1);
  put16(f, 0);
  f.push_back(0);
  f.push_back(2);
  putstr(f, "Skyrim.esm");
  putstr(f, "SkyUI_SE.esp");
  put16(f, 1);
  putstr(f, "Light.esp");
  write_file_bytes(p, f);

  const SaveGame g = parse_gamebryo_tesv_fast(p, "skyrimse");
  check(g.pc_name == "Vanny", "name");
  check(g.pc_level == 40, "level");
  check(g.pc_location == "Whiterun", "location");
  check(g.save_number == 7, "save number");
  check(g.creation_time == filetime_to_epoch(kFt), "filetime conversion");
  check(g.plugins == std::vector<std::string>{"Skyrim.esm", "SkyUI_SE.esp"}, "plugins");
  check(g.light_plugins == std::vector<std::string>{"Light.esp"},
        "light plugins (version >= 78 gate)");
  check(g.screenshot.empty(), "no screenshot bytes retained");
  check(!g.has_heavy_data, "hover re-parses on demand");
  check(g.game_id == "skyrimse", "game tag propagates");
  check(g.file_path == p, "path propagates");

  fs::remove_all(root);
}

TEST_CASE("gamebryo fast scan: se type-1 multi-chunk early stop", "[engine]") {
  const fs::path root = fs::temp_directory_path() / "gmm_fast_se1";
  fs::remove_all(root);
  fs::create_directories(root);
  const fs::path p = root / "C.ess";

  constexpr std::uint64_t kFt = 0x01DD227000000000ULL;
  std::vector<char> f;
  write_tesv_head(f, 12, 3, "Early", 12, "Riften", kFt);
  put32(f, 320);
  put32(f, 192);
  put16(f, 1);  // compression type 1
  for (int i = 0; i < 320 * 192 * 4; ++i)
    f.push_back(static_cast<char>(i & 0xFF));  // full-size shot (skipped)
  // Region: plugin head, then ~2MiB of filler across many streams, then
  // GARBAGE where the tail chunks would be. The fast reader must never
  // touch anything past the lists; a full inflate would choke on it.
  std::vector<char> raw;
  put_se_region_head(raw, {"Skyrim.esm", "A.esp"}, {"B.esl"});
  while (raw.size() < 2 * 1024 * 1024)
    raw.push_back(static_cast<char>(raw.size() & 0xFF));
  append_type1(f, raw);
  for (int i = 0; i < 65536; ++i)
    f.push_back(static_cast<char>(0xCC));  // unaligned garbage tail
  write_file_bytes(p, f);

  const SaveGame g = parse_gamebryo_tesv_fast(p, "skyrimse");
  check(g.pc_name == "Early", "name past the skipped shot");
  check(g.pc_location == "Riften", "location");
  check(g.plugins == std::vector<std::string>{"Skyrim.esm", "A.esp"},
        "plugins from the first chunk");
  check(g.light_plugins == std::vector<std::string>{"B.esl"}, "light list");
  check(g.screenshot.empty() && !g.has_heavy_data, "still heavy-free");

  fs::remove_all(root);
}

TEST_CASE("gamebryo fast scan: le layout without light plugins", "[engine]") {
  const fs::path root = fs::temp_directory_path() / "gmm_fast_le";
  fs::remove_all(root);
  fs::create_directories(root);
  const fs::path p = root / "LE.ess";

  constexpr std::uint64_t kFt = 0x01DD228000000000ULL;
  std::vector<char> f;
  write_tesv_head(f, 9, 11, "Oldie", 60, "Solitude", kFt);
  put32(f, 16);
  put32(f, 16);
  // LE: RGB shot, NO compression-type field.
  for (int i = 0; i < 16 * 16 * 3; ++i)
    f.push_back(static_cast<char>(i & 0xFF));
  // LE region: skip1 form version, skip u32 info size, u8 count + names.
  f.push_back(77);  // form version (skipped)
  put32(f, 99);     // plugin info size (skipped, u32 in LE)
  f.push_back(1);
  putstr(f, "Skyrim.esm");
  write_file_bytes(p, f);

  const SaveGame g = parse_gamebryo_tesv_fast(p, "skyrim");
  check(g.pc_name == "Oldie", "LE name");
  check(g.plugins == std::vector<std::string>{"Skyrim.esm"}, "LE plugins");
  check(g.light_plugins.empty(), "LE never reads light plugins");
  check(!g.has_heavy_data, "heavy-free");

  fs::remove_all(root);
}

TEST_CASE("gamebryo fast scan: rejects bad magic and truncated heads", "[engine]") {
  const fs::path root = fs::temp_directory_path() / "gmm_fast_bad";
  fs::remove_all(root);
  fs::create_directories(root);

  write_file(root / "bad.ess", "definitely not a save");
  bool threw = false;
  try {
    const SaveGame rejected = parse_gamebryo_tesv_fast(root / "bad.ess", "skyrimse");
    (void)rejected;
  } catch (const SaveParseError &) {
    threw = true;
  }
  check(threw, "wrong magic -> SaveParseError (scan skips the file)");

  // Truncated right after the magic: header reads run off the end.
  std::vector<char> tiny;
  const char *magic = "TESV_SAVEGAME";
  tiny.insert(tiny.end(), magic, magic + 13);
  put32(tiny, 0);
  write_file_bytes(root / "tiny.ess", tiny);
  threw = false;
  try {
    const SaveGame rejected = parse_gamebryo_tesv_fast(root / "tiny.ess", "skyrimse");
    (void)rejected;
  } catch (const SaveParseError &) {
    threw = true;
  }
  check(threw, "truncated header -> SaveParseError");

  // Hostile screenshot dimensions (w*h overflows past the file): the
  // multiply is guarded, so this is a clean reject, not a huge alloc.
  std::vector<char> evil;
  write_tesv_head(evil, 12, 1, "E", 1, "L", 0x01DD228000000000ULL);
  put32(evil, 0xFFFFFFFFu);
  put32(evil, 0xFFFFFFFFu);
  put16(evil, 1);
  write_file_bytes(root / "evil.ess", evil);
  threw = false;
  try {
    const SaveGame rejected = parse_gamebryo_tesv_fast(root / "evil.ess", "skyrimse");
    (void)rejected;
  } catch (const SaveParseError &) {
    threw = true;
  }
  check(threw, "absurd screenshot size -> SaveParseError, no huge alloc");

  fs::remove_all(root);
}

TEST_CASE("gamebryo fast scan: oversized plugin data asks for full parse", "[engine]") {
  // A save whose light-plugin list alone exceeds the decompressed cap
  // cannot be served cheaply: the fast reader throws SaveNeedFullParse
  // (an IS-A SaveParseError, so outer skip paths stay safe) and the
  // worker reruns that file through the full parser instead of dropping
  // the row.
  const fs::path root = fs::temp_directory_path() / "gmm_fast_big";
  fs::remove_all(root);
  fs::create_directories(root);
  const fs::path p = root / "Big.ess";

  std::vector<char> f;
  write_tesv_head(f, 12, 9, "Hoarder", 99, "Everywhere", 0x01DD228800000000ULL);
  put32(f, 8);
  put32(f, 8);
  put16(f, 1);  // compression type 1: the cap truncates the inflate
  for (int i = 0; i < 8 * 8 * 4; ++i)
    f.push_back(0);
  std::vector<char> raw;
  raw.push_back(78);
  raw.push_back(1);
  put16(raw, 0);
  raw.push_back(0);
  raw.push_back(1);
  putstr(raw, "Skyrim.esm");
  // 9000 light names x ~50B = ~450KiB of lists, past the 256KiB
  // decompressed cap, so the capped inflate runs dry mid-list.
  const std::string pad(48, 'x');
  put16(raw, 9000);
  for (int i = 0; i < 9000; ++i)
    putstr(raw, "Mod" + std::to_string(i) + pad + ".esl");
  append_type1(f, raw);
  write_file_bytes(p, f);

  bool need_full      = false;
  bool is_parse_error = false;
  try {
    const SaveGame rejected = parse_gamebryo_tesv_fast(p, "skyrimse");
    (void)rejected;
  } catch (const SaveNeedFullParse &) {
    need_full      = true;
    is_parse_error = true;
  } catch (const SaveParseError &) {
    is_parse_error = true;
  }
  check(need_full, "past-cap lists throw SaveNeedFullParse");
  check(is_parse_error, "SaveNeedFullParse is a SaveParseError");

  fs::remove_all(root);
}
