// Tests for engine::Collection::Nexus::parse — the Nexus collection.json parser.
// Verifies mapping from Nexus Vortex-compatible collection JSON into the
// source-agnostic Collection::Manifest data model.

#include "engine/collection/nexus/parser.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <variant>

using namespace engine::Collection;
using namespace engine::Collection::Nexus;

// ---------------------------------------------------------------------------
// Minimal valid collection
// ---------------------------------------------------------------------------

TEST_CASE("parse minimal valid collection.json", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {
            "name": "Test Pack",
            "author": "Tester",
            "description": "A test",
            "domainName": "skyrimspecialedition"
        },
        "mods": []
    })");

    REQUIRE(m.info.name == "Test Pack");
    REQUIRE(m.info.author == "Tester");
    REQUIRE(m.info.description == "A test");
    REQUIRE(m.info.game_id == "skyrimspecialedition");
    REQUIRE(m.mods.empty());
    REQUIRE(m.rules.empty());
}

// ---------------------------------------------------------------------------
// Info fields
// ---------------------------------------------------------------------------

TEST_CASE("parse info populates PackInfo", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {
            "name": "My Collection",
            "author": "Author1",
            "description": "Desc",
            "domainName": "fallout4",
            "authorUrl": "https://example.com"
        },
        "mods": []
    })");

    REQUIRE(m.info.name == "My Collection");
    REQUIRE(m.info.author == "Author1");
    REQUIRE(m.info.description == "Desc");
    REQUIRE(m.info.game_id == "fallout4");
    REQUIRE(m.info.homepage == "https://example.com");
}

TEST_CASE("parse info tolerates missing optional fields", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": []
    })");

    REQUIRE(m.info.name.empty());
    REQUIRE(m.info.author.empty());
    REQUIRE(m.info.game_id.empty());
}

TEST_CASE("schema_version from top-level version field", "[collection][nexus]") {
    auto m = parse(R"({
        "info": {},
        "version": "1.2.3",
        "mods": []
    })");

    REQUIRE(m.schema_version == "1.2.3");
}

TEST_CASE("schema_version fallback to schemaVersion", "[collection][nexus]") {
    auto m = parse(R"({
        "info": {},
        "schemaVersion": "2.0.0",
        "mods": []
    })");

    REQUIRE(m.schema_version == "2.0.0");
}

TEST_CASE("schema_version defaults to nexus/1 when absent", "[collection][nexus]") {
    auto m = parse(R"({
        "info": {},
        "mods": []
    })");

    REQUIRE(m.schema_version == "nexus/1");
}

// ---------------------------------------------------------------------------
// Mod parsing — Nexus source type
// ---------------------------------------------------------------------------

TEST_CASE("parse single nexus mod", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": { "domainName": "skyrimspecialedition" },
        "mods": [{
            "name": "SkyUI",
            "version": "1.4.2",
            "optional": false,
            "phase": 1,
            "source": {
                "type": "nexus",
                "modId": 3863,
                "fileId": 10001,
                "fileSize": 48213311,
                "logicalFilename": "SkyUI_1_2_masterfile.7z",
                "md5": "abc123"
            }
        }]
    })");

    REQUIRE(m.mods.size() == 1);
    const auto& mod = m.mods[0];
    REQUIRE(mod.name == "SkyUI");
    REQUIRE(mod.id == "skyui"); // slugified from name
    REQUIRE(mod.phase == 1);
    REQUIRE(mod.category == ModCategory::Required);
    REQUIRE_FALSE(mod.installer_choices.type.empty() == false); // empty

    REQUIRE(std::holds_alternative<SourceNexus>(mod.source));
    const auto& src = std::get<SourceNexus>(mod.source);
    REQUIRE(src.game_domain == "skyrimspecialedition");
    REQUIRE(src.mod_id == 3863);
    REQUIRE(src.file_id == 10001);
    REQUIRE(src.file_size == 48213311);
    REQUIRE(src.file_name == "SkyUI_1_2_masterfile.7z");
    REQUIRE(src.sha256 == "abc123");
    REQUIRE(src.update_policy == UpdatePolicy::Exact);
    REQUIRE(src.resolution == SourceResolution::Api);
}

TEST_CASE("parse optional mod", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": [{
            "name": "Optional ENB",
            "optional": true,
            "source": { "type": "nexus" }
        }]
    })");

    REQUIRE(m.mods.size() == 1);
    REQUIRE(m.mods[0].category == ModCategory::Optional);
}

TEST_CASE("mod id is slugified from name", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": [{
            "name": "Skyrim 2020  Textures",
            "source": { "type": "nexus" }
        }]
    })");

    REQUIRE(m.mods[0].id == "skyrim-2020-textures");
}

TEST_CASE("game domain falls back to info.domainName", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": { "domainName": "oblivion" },
        "mods": [{
            "name": "Test Mod",
            "source": { "type": "nexus", "modId": 100 }
        }]
    })");

    REQUIRE(std::holds_alternative<SourceNexus>(m.mods[0].source));
    REQUIRE(std::get<SourceNexus>(m.mods[0].source).game_domain == "oblivion");
}

TEST_CASE("source.gameDomain overrides info.domainName", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": { "domainName": "oblivion" },
        "mods": [{
            "name": "Cross-Game Mod",
            "source": { "type": "nexus", "gameDomain": "morrowind", "modId": 200 }
        }]
    })");

    REQUIRE(std::get<SourceNexus>(m.mods[0].source).game_domain == "morrowind");
}

// ---------------------------------------------------------------------------
// Mod parsing — fileExpression fallback
// ---------------------------------------------------------------------------

TEST_CASE("fileExpression fallback when logicalFilename absent", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": [{
            "name": "Legacy Mod",
            "source": { "type": "nexus", "fileExpression": "legacy_v1.zip" }
        }]
    })");

    REQUIRE(std::get<SourceNexus>(m.mods[0].source).file_name == "legacy_v1.zip");
}

// ---------------------------------------------------------------------------
// Mod parsing — update policy
// ---------------------------------------------------------------------------

TEST_CASE("updatePolicy=latest on mod", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": [{
            "name": "SKSE",
            "source": { "type": "nexus", "updatePolicy": "latest" }
        }]
    })");

    REQUIRE(std::get<SourceNexus>(m.mods[0].source).update_policy == UpdatePolicy::Latest);
}

TEST_CASE("updatePolicy defaults to exact", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": [{
            "name": "Static Mod",
            "source": { "type": "nexus" }
        }]
    })");

    REQUIRE(std::get<SourceNexus>(m.mods[0].source).update_policy == UpdatePolicy::Exact);
}

// ---------------------------------------------------------------------------
// Mod parsing — different source types
// ---------------------------------------------------------------------------

TEST_CASE("parse mod with browse/manual source", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": [{
            "name": "Manual Download",
            "source": {
                "type": "browse",
                "url": "https://example.com/mod.zip",
                "logicalFilename": "manual.zip"
            }
        }]
    })");

    REQUIRE(std::holds_alternative<SourceDirect>(m.mods[0].source));
    const auto& src = std::get<SourceDirect>(m.mods[0].source);
    REQUIRE(src.resolution == SourceResolution::Browser);
    REQUIRE(src.url == "https://example.com/mod.zip");
    REQUIRE(src.file_name == "manual.zip");
}

TEST_CASE("parse mod with bundle source", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": [{
            "name": "Bundled Patch",
            "source": { "type": "bundle" }
        }]
    })");

    REQUIRE(std::holds_alternative<SourceDirect>(m.mods[0].source));
}

TEST_CASE("parse mod with unknown source type defaults to Direct", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": [{
            "name": "Future Mod",
            "source": { "type": "steam_workshop" }
        }]
    })");

    REQUIRE(std::holds_alternative<SourceDirect>(m.mods[0].source));
}

// ---------------------------------------------------------------------------
// Installer choices (FOMOD replay)
// ---------------------------------------------------------------------------

TEST_CASE("parse mod with installer choices", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": [{
            "name": "FOMOD Mod",
            "source": { "type": "nexus" },
            "choices": {
                "Step1-Group1": ["OptionA", "OptionC"],
                "Step2-Group1": ["OptionB"]
            }
        }]
    })");

    const auto& ic = m.mods[0].installer_choices;
    REQUIRE(ic.type == "fomod");
    REQUIRE(ic.selections.size() == 2);
    REQUIRE(ic.selections.at("Step1-Group1").size() == 2);
    REQUIRE(ic.selections.at("Step1-Group1")[0] == "OptionA");
    REQUIRE(ic.selections.at("Step1-Group1")[1] == "OptionC");
    REQUIRE(ic.selections.at("Step2-Group1").size() == 1);
}

// ---------------------------------------------------------------------------
// Mod rules
// ---------------------------------------------------------------------------

TEST_CASE("parse modRules into install rules", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": [],
        "modRules": [
            { "sourceMod": "skse", "targetMod": "engine-fixes", "type": "requires" },
            { "sourceMod": "textures", "targetMod": "enb", "type": "after" },
            { "sourceMod": "conflict-a", "targetMod": "conflict-b", "type": "conflicts" },
            { "sourceMod": "before-mod", "targetMod": "after-mod", "type": "before" }
        ]
    })");

    REQUIRE(m.rules.size() == 4);

    REQUIRE(m.rules[0].type == RuleType::Requires);
    REQUIRE(m.rules[0].from == "skse");
    REQUIRE(m.rules[0].to == "engine-fixes");

    REQUIRE(m.rules[1].type == RuleType::After);
    REQUIRE(m.rules[1].from == "textures");
    REQUIRE(m.rules[1].to == "enb");

    REQUIRE(m.rules[2].type == RuleType::Conflicts);
    REQUIRE(m.rules[2].from == "conflict-a");
    REQUIRE(m.rules[2].to == "conflict-b");

    REQUIRE(m.rules[3].type == RuleType::Before);
    REQUIRE(m.rules[3].from == "before-mod");
    REQUIRE(m.rules[3].to == "after-mod");
}

TEST_CASE("parse modRules defaults to Before for unknown type", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": [],
        "modRules": [
            { "sourceMod": "a", "targetMod": "b", "type": "something_weird" }
        ]
    })");

    REQUIRE(m.rules.size() == 1);
    REQUIRE(m.rules[0].type == RuleType::Before);
}

TEST_CASE("modRules with empty source/target are skipped", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": [],
        "modRules": [
            { "sourceMod": "", "targetMod": "b", "type": "before" },
            { "sourceMod": "a", "targetMod": "", "type": "before" }
        ]
    })");

    REQUIRE(m.rules.empty());
}

// ---------------------------------------------------------------------------
// Plugin load order
// ---------------------------------------------------------------------------

TEST_CASE("parse pluginLoadOrder", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": [],
        "pluginLoadOrder": ["Unofficial Patch.esp", "SkyUI.esp", "SMIM.esp"]
    })");

    REQUIRE(m.load_order.plugin_hint.size() == 3);
    REQUIRE(m.load_order.plugin_hint[0] == "Unofficial Patch.esp");
    REQUIRE(m.load_order.plugin_hint[1] == "SkyUI.esp");
    REQUIRE(m.load_order.plugin_hint[2] == "SMIM.esp");
}

TEST_CASE("pluginLoadOrder ignores non-string entries", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": [],
        "pluginLoadOrder": ["valid.esp", 42, null, true]
    })");

    REQUIRE(m.load_order.plugin_hint.size() == 1);
    REQUIRE(m.load_order.plugin_hint[0] == "valid.esp");
}

// ---------------------------------------------------------------------------
// Multiple mods
// ---------------------------------------------------------------------------

TEST_CASE("parse multiple mods preserves order", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": [
            { "name": "Mod A", "source": { "type": "nexus" } },
            { "name": "Mod B", "source": { "type": "nexus" } },
            { "name": "Mod C", "source": { "type": "browse" } }
        ]
    })");

    REQUIRE(m.mods.size() == 3);
    REQUIRE(m.mods[0].name == "Mod A");
    REQUIRE(m.mods[1].name == "Mod B");
    REQUIRE(m.mods[2].name == "Mod C");
    REQUIRE(std::holds_alternative<SourceNexus>(m.mods[0].source));
    REQUIRE(std::holds_alternative<SourceDirect>(m.mods[2].source));
}

// ---------------------------------------------------------------------------
// Non-object mods are skipped
// ---------------------------------------------------------------------------

TEST_CASE("non-object entries in mods array are skipped", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": [null, 42, "string", { "name": "Real Mod", "source": { "type": "nexus" } }]
    })");

    REQUIRE(m.mods.size() == 1);
    REQUIRE(m.mods[0].name == "Real Mod");
}

// ---------------------------------------------------------------------------
// Non-object modRules are skipped
// ---------------------------------------------------------------------------

TEST_CASE("non-object entries in modRules are skipped", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": [],
        "modRules": [null, 42, { "sourceMod": "a", "targetMod": "b", "type": "before" }]
    })");

    REQUIRE(m.rules.size() == 1);
}

// ---------------------------------------------------------------------------
// Empty collection
// ---------------------------------------------------------------------------

TEST_CASE("parse empty JSON object", "[collection][nexus]") {
    const auto m = parse(R"({})");

    REQUIRE(m.info.name.empty());
    REQUIRE(m.mods.empty());
    REQUIRE(m.rules.empty());
    REQUIRE(m.load_order.plugin_hint.empty());
    REQUIRE(m.schema_version == "nexus/1");
}

// ---------------------------------------------------------------------------
// Error cases
// ---------------------------------------------------------------------------

TEST_CASE("parse throws on invalid JSON", "[collection][nexus]") {
    REQUIRE_THROWS_AS(parse("{broken json}"), ParseError);
}

TEST_CASE("parse throws on non-object root", "[collection][nexus]") {
    REQUIRE_THROWS_AS(parse("[]"), ParseError);
    REQUIRE_THROWS_AS(parse("\"string\""), ParseError);
    REQUIRE_THROWS_AS(parse("42"), ParseError);
    REQUIRE_THROWS_AS(parse("null"), ParseError);
}

TEST_CASE("parse throws on mod missing source", "[collection][nexus]") {
    REQUIRE_THROWS_AS(parse(R"({
        "info": {},
        "mods": [{ "name": "No Source Mod" }]
    })"), ParseError);
}

TEST_CASE("parse throws on mod missing name", "[collection][nexus]") {
    REQUIRE_THROWS_AS(parse(R"({
        "info": {},
        "mods": [{ "source": { "type": "nexus" } }]
    })"), ParseError);
}

TEST_CASE("parse throws on mod source not an object", "[collection][nexus]") {
    REQUIRE_THROWS_AS(parse(R"({
        "info": {},
        "mods": [{ "name": "Bad Source", "source": "nexus" }]
    })"), ParseError);
}

// ---------------------------------------------------------------------------
// Full realistic Nexus collection.json
// ---------------------------------------------------------------------------

TEST_CASE("parse realistic Nexus collection", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {
            "name": "Ultimate Skyrim SE Overhaul",
            "author": "PackAuthor",
            "description": "A comprehensive modding guide as a collection.",
            "domainName": "skyrimspecialedition",
            "authorUrl": "https://next.nexusmods.com/skyrimspecialedition/collections/abc123"
        },
        "version": "3.1.0",
        "mods": [
            {
                "name": "SKSE64",
                "version": "2.2.6",
                "phase": 0,
                "optional": false,
                "source": {
                    "type": "nexus",
                    "modId": 3018,
                    "fileId": 40160,
                    "fileSize": 1234567,
                    "logicalFilename": "skse64_2_02_06.7z",
                    "md5": "deadbeef",
                    "updatePolicy": "latest"
                }
            },
            {
                "name": "SkyUI",
                "version": "1.4.2",
                "phase": 1,
                "optional": false,
                "source": {
                    "type": "nexus",
                    "modId": 3863,
                    "fileId": 10001,
                    "fileSize": 48213311,
                    "logicalFilename": "SkyUI_1_2_masterfile.7z",
                    "md5": "cafebabe"
                }
            },
            {
                "name": "ENB Preset",
                "version": "2.0",
                "phase": 3,
                "optional": true,
                "source": {
                    "type": "browse",
                    "url": "https://enbdev.com/enbseries/download.html",
                    "logicalFilename": "enbseries.zip"
                },
                "choices": {
                    "ENB-Color": ["Natural"],
                    "ENB-Presets": ["Rudy", "Silent"]
                }
            }
        ],
        "modRules": [
            { "sourceMod": "SkyUI", "targetMod": "SKSE64", "type": "requires" },
            { "sourceMod": "ENB Preset", "targetMod": "SkyUI", "type": "after" }
        ],
        "pluginLoadOrder": [
            "skse64_loader.exe",
            "SkyUI_SE.esp"
        ]
    })");

    // Info
    REQUIRE(m.info.name == "Ultimate Skyrim SE Overhaul");
    REQUIRE(m.info.author == "PackAuthor");
    REQUIRE(m.info.game_id == "skyrimspecialedition");
    REQUIRE(m.info.homepage == "https://next.nexusmods.com/skyrimspecialedition/collections/abc123");

    // Schema version
    REQUIRE(m.schema_version == "3.1.0");

    // Mods
    REQUIRE(m.mods.size() == 3);

    // Mod 0: SKSE64
    REQUIRE(m.mods[0].id == "skse64");
    REQUIRE(m.mods[0].phase == 0);
    REQUIRE(m.mods[0].category == ModCategory::Required);
    REQUIRE(std::holds_alternative<SourceNexus>(m.mods[0].source));
    const auto& skse = std::get<SourceNexus>(m.mods[0].source);
    REQUIRE(skse.mod_id == 3018);
    REQUIRE(skse.update_policy == UpdatePolicy::Latest);

    // Mod 1: SkyUI
    REQUIRE(m.mods[1].id == "skyui");
    REQUIRE(m.mods[1].phase == 1);
    REQUIRE(std::holds_alternative<SourceNexus>(m.mods[1].source));
    const auto& skyui = std::get<SourceNexus>(m.mods[1].source);
    REQUIRE(skyui.mod_id == 3863);
    REQUIRE(skyui.file_id == 10001);
    REQUIRE(skyui.update_policy == UpdatePolicy::Exact);

    // Mod 2: ENB Preset (browse/manual)
    REQUIRE(m.mods[2].id == "enb-preset");
    REQUIRE(m.mods[2].category == ModCategory::Optional);
    REQUIRE(std::holds_alternative<SourceDirect>(m.mods[2].source));
    const auto& enb = std::get<SourceDirect>(m.mods[2].source);
    REQUIRE(enb.url == "https://enbdev.com/enbseries/download.html");

    // Installer choices on ENB
    REQUIRE(m.mods[2].installer_choices.type == "fomod");
    REQUIRE(m.mods[2].installer_choices.selections.size() == 2);

    // Rules
    REQUIRE(m.rules.size() == 2);
    REQUIRE(m.rules[0].type == RuleType::Requires);
    REQUIRE(m.rules[0].from == "SkyUI");
    REQUIRE(m.rules[0].to == "SKSE64");
    REQUIRE(m.rules[1].type == RuleType::After);

    // Plugin load order
    REQUIRE(m.load_order.plugin_hint.size() == 2);
    REQUIRE(m.load_order.plugin_hint[0] == "skse64_loader.exe");
}

// ---------------------------------------------------------------------------
// Phase default
// ---------------------------------------------------------------------------

TEST_CASE("mod phase defaults to 0", "[collection][nexus]") {
    const auto m = parse(R"({
        "info": {},
        "mods": [{ "name": "Default Phase", "source": { "type": "nexus" } }]
    })");

    REQUIRE(m.mods[0].phase == 0);
}

// ---------------------------------------------------------------------------
// SourceNexus defaults
// ---------------------------------------------------------------------------

TEST_CASE("SourceNexus defaults", "[collection][nexus]") {
    SourceNexus nx;
    REQUIRE(nx.mod_id == 0);
    REQUIRE(nx.file_id == 0);
    REQUIRE(nx.file_size == 0);
    REQUIRE(nx.update_policy == UpdatePolicy::Exact);
    REQUIRE_FALSE(nx.game_domain.empty() == false); // empty by default
}

// ---------------------------------------------------------------------------
// parse_file
// ---------------------------------------------------------------------------

TEST_CASE("parse_file with valid temp file", "[collection][nexus]") {
    // Write a temp collection.json
    const std::string path = "test_nexus_collection.json";
    {
        std::ofstream f(path);
        f << R"({
            "info": { "name": "File Test", "domainName": "skyrim" },
            "mods": [{ "name": "File Mod", "source": { "type": "nexus" } }]
        })";
    }

    const auto m = parse_file(path);
    REQUIRE(m.info.name == "File Test");
    REQUIRE(m.mods.size() == 1);
    REQUIRE(m.mods[0].name == "File Mod");

    std::remove(path.c_str());
}

TEST_CASE("parse_file throws on nonexistent file", "[collection][nexus]") {
    REQUIRE_THROWS_AS(parse_file("nonexistent_file_abc123.json"), ParseError);
}
