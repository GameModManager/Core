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

#include "engine/game/saves/save_game.h"
#include "engine/game/saves/save_reader.h"
#include "engine/game/saves/save_missing_assets.h"
#include "engine/game/saves/save_scanner.h"
#include "engine/pipeline/plugin_host/save_parser_registry.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

using namespace engine;

namespace fs = std::filesystem;

namespace {
void check(bool cond, const char* what) {
    INFO(what);
    REQUIRE(cond);
}

void write_file(const fs::path& p, const std::string& data) {
    std::ofstream(p, std::ios::binary).write(data.data(),
                                             static_cast<std::streamsize>(data.size()));
}

// Touch a file with a given mtime (used to give two saves a known
// newest-first ordering without depending on real SE file format).
void touch_with_mtime(const fs::path& p, std::chrono::seconds offset) {
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
    auto& reg = SaveParserRegistry::instance();

    check(!reg.has_parser("stubgame"), "no parser registered initially");

    int a_calls = 0;
    int b_calls = 0;
    reg.register_parser(
        "stubgame", 10,
        [&a_calls](const fs::path&, const std::string& gid) {
            ++a_calls;
            SaveGame g;
            g.game_id = gid;
            g.pc_name = "from-A";
            return g;
        },
        nullptr, "pluginA");

    reg.register_parser(
        "stubgame", 100,
        [&b_calls](const fs::path&, const std::string& gid) {
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
    auto& reg = SaveParserRegistry::instance();

    reg.register_parser("g1", 50, nullptr, nullptr, "pluginX");
    check(!reg.has_parser("g1"), "null fn is ignored, no entry added");

    reg.register_parser(
        "g1", 50,
        [](const fs::path&, const std::string&) { return SaveGame{}; },
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
    int call_count = 0;
    SaveParseFn parse_fn = [&call_count](const fs::path& p) {
        ++call_count;
        if (p.filename() == "bad.ess") {
            throw SaveParseError("synthetic");
        }
        SaveGame g;
        g.file_path = p;
        g.game_id = "stubgame";
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
        check(saves[i - 1].creation_time >= saves[i].creation_time,
              "newest first");
    }
    check(saves.front().file_path.filename() == "new.ess",
          "newest file lands first");

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
    SaveParseFn parses = [gid](const fs::path& p) {
        if (SaveParserRegistry::instance().has_parser(gid)) {
            auto r = SaveParserRegistry::instance().parse_save(p, gid);
            if (!r) throw SaveParseError("no save parser for " + gid);
            return *r;
        }
        SaveGame stub;
        stub.file_path = p;
        stub.game_id = gid;
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
    auto& reg = SaveParserRegistry::instance();
    const std::string gid = "e2egame";

    int parsed = 0;
    reg.register_parser(
        gid, 50,
        [&parsed](const fs::path& p, const std::string& g) {
            ++parsed;
            SaveGame s;
            s.file_path = p;
            s.game_id = g;
            s.pc_name = "E2E";
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
    SaveParseFn parses = [gid](const fs::path& p) {
        auto r = SaveParserRegistry::instance().parse_save(p, gid);
        if (!r) throw SaveParseError("no save parser for " + gid);
        return *r;
    };

    auto saves = scan_saves(root, {"ess"}, parses);
    check(saves.size() == 2, "registry parser sees both .ess files");
    check(parsed == 2, "parser invoked twice");
    for (const auto& s : saves) {
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
        p.name = "Skyrim.esm";
        p.enabled = true;
        p.owner_mod = "";
        plugins.push_back(p);
    }
    {
        GamePlugin p;
        p.name = "SkyUI_SE.esp";
        p.enabled = false;
        p.owner_mod = "SkyUI";
        plugins.push_back(p);
    }
    {
        GamePlugin p;
        p.name = "SomeOtherMod.esp";
        p.enabled = true;
        p.owner_mod = "SomeOtherMod";
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
    save.plugins = {"Skyrim.esm", "SkyUI_SE.esp", "GonePlugin.esp",
                    "AlsoMissing.esm"};
    auto missing = find_save_missing_assets(save, plugins, am, ow);

    check(missing.size() == 3, "missing asset count");
    std::map<std::string, const SaveMissingAsset*> by_name;
    for (const auto& m : missing) by_name[m.plugin_name] = &m;

    auto it = by_name.find("SkyUI_SE.esp");
    check(it != by_name.end(), "inactive plugin is missing");
    check(it->second->inactive, "inactive flag set");
    check(it->second->origin_mod == "SkyUI", "inactive origin mod");
    check(std::find(it->second->providing_mods.begin(),
                    it->second->providing_mods.end(),
                    "SkyUI") != it->second->providing_mods.end(),
          "inactive provider found");

    it = by_name.find("GonePlugin.esp");
    check(it != by_name.end(), "absent plugin is missing");
    check(!it->second->inactive, "absent plugin not flagged inactive");
    check(it->second->origin_mod.empty(), "absent plugin has no origin");
    check(std::find(it->second->providing_mods.begin(),
                    it->second->providing_mods.end(),
                    "GoneMod") != it->second->providing_mods.end(),
          "providing mod found");
    check(std::find(it->second->providing_mods.begin(),
                    it->second->providing_mods.end(),
                    "<overwrite>") != it->second->providing_mods.end(),
          "overwrite provider found");

    it = by_name.find("AlsoMissing.esm");
    check(it != by_name.end(), "second absent plugin is missing");
    check(it->second->providing_mods.empty(), "no provider for AlsoMissing");

    check(by_name.find("Skyrim.esm") == by_name.end(),
          "active plugin is not missing");

    fs::remove_all(root);
}
