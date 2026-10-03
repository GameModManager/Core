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
  const auto &mod = m.mods[0];
  REQUIRE(mod.name == "SkyUI");
  REQUIRE(mod.id == "skyui");  // slugified from name
  REQUIRE(mod.phase == 1);
  REQUIRE(mod.category == ModCategory::Required);
  REQUIRE_FALSE(mod.installer_choices.type.empty() == false);  // empty

  REQUIRE(std::holds_alternative<SourceNexus>(mod.source));
  const auto &src = std::get<SourceNexus>(mod.source);
  REQUIRE(src.game_domain == "skyrimspecialedition");
  REQUIRE(src.mod_id == 3863);
  REQUIRE(src.file_id == 10001);
  REQUIRE(src.file_size == 48213311);
  REQUIRE(src.file_name == "SkyUI_1_2_masterfile.7z");
  // Nexus publishes an md5, and it stays in the md5 field. Nothing here may
  // claim a sha256 the source never supplied: an exact pin is defined by that
  // 64-hex digest, and a 32-hex md5 in a sha256 field satisfies it falsely.
  REQUIRE(src.md5 == "abc123");
  REQUIRE(src.sha256.empty());
  REQUIRE(src.update_policy == UpdatePolicy::Prefer);
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

TEST_CASE("fileExpression fallback when logicalFilename absent",
          "[collection][nexus]") {
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

  REQUIRE(std::get<SourceNexus>(m.mods[0].source).update_policy ==
          UpdatePolicy::Latest);
}

TEST_CASE("updatePolicy=prefer stays prefer, never collapses to exact",
          "[collection][nexus]") {
  const auto m = parse(R"({
        "info": {},
        "mods": [{
            "name": "Pinned Mod",
            "source": { "type": "nexus", "fileId": 10001, "updatePolicy": "prefer" }
        }]
    })");

  const auto &src = std::get<SourceNexus>(m.mods[0].source);
  REQUIRE(src.update_policy == UpdatePolicy::Prefer);
  REQUIRE(src.update_policy != UpdatePolicy::Exact);
  REQUIRE(src.file_id == 10001);  // the pin survives
}

TEST_CASE("updatePolicy=exact degrades to prefer with no sha256 to back it",
          "[collection][nexus]") {
  // Nexus supplies an md5 and never a sha256, so an exact pin it declares
  // cannot be verified. The pin is kept and the fallback is allowed rather
  // than emitting a pin backed by a digest we would have to invent.
  const auto m = parse(R"({
        "info": {},
        "mods": [{
            "name": "Pinned Mod",
            "source": {
                "type": "nexus", "fileId": 10001, "fileSize": 4096,
                "md5": "0123456789abcdef0123456789abcdef",
                "updatePolicy": "exact"
            }
        }]
    })");

  const auto &src = std::get<SourceNexus>(m.mods[0].source);
  REQUIRE(src.update_policy == UpdatePolicy::Prefer);
  REQUIRE(src.md5 == "0123456789abcdef0123456789abcdef");
  REQUIRE(src.sha256.empty());
}

TEST_CASE("absent updatePolicy is not an exact pin", "[collection][nexus]") {
  const auto m = parse(R"({
        "info": {},
        "mods": [{
            "name": "Static Mod",
            "source": { "type": "nexus" }
        }]
    })");

  REQUIRE(std::get<SourceNexus>(m.mods[0].source).update_policy ==
          UpdatePolicy::Prefer);
}

TEST_CASE("update policy round-trips through its own string form",
          "[collection][nexus]") {
  REQUIRE(std::string(to_string(UpdatePolicy::Exact)) == "exact");
  REQUIRE(std::string(to_string(UpdatePolicy::Prefer)) == "prefer");
  REQUIRE(std::string(to_string(UpdatePolicy::Latest)) == "latest");
  for (auto policy :
       {UpdatePolicy::Exact, UpdatePolicy::Prefer, UpdatePolicy::Latest}) {
    REQUIRE(parse_update_policy(to_string(policy)) == policy);
  }
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
  const auto &src = std::get<SourceDirect>(m.mods[0].source);
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

TEST_CASE("parse mod with unknown source type defaults to Direct",
          "[collection][nexus]") {
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

  const auto &ic = m.mods[0].installer_choices;
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

// Both ends of a rule are Vortex references, not names, and each resolves to
// the id of a mod in the collection. Every type the format emits appears here,
// and each rule picks a different resolution key. The mod list carries the
// fields all five keys read: md5, fileSize, version and the archive name.
TEST_CASE("parse modRules into install rules", "[collection][nexus]") {
  const auto m = parse(R"({
        "info": {},
        "mods": [
            { "name": "SKSE64", "version": "2.2.6",
              "source": { "type": "nexus", "modId": 3018, "fileId": 40160,
                          "fileSize": 1234567, "logicalFilename": "skse64_2_02_06.7z",
                          "md5": "3d1a4b1e2c5f60718293a4b5c6d7e8f90",
                          "updatePolicy": "latest" } },
            { "name": "SkyUI", "version": "1.4.2",
              "source": { "type": "nexus", "modId": 3863, "fileId": 10001,
                          "fileSize": 48213311,
                          "logicalFilename": "SkyUI_1_2_masterfile.7z",
                          "md5": "cafebabecafebabecafebabecafebabe" } },
            { "name": "Engine Fixes", "version": "3.4.1",
              "source": { "type": "nexus", "modId": 31415, "fileId": 55001,
                          "fileSize": 999, "logicalFilename": "engine_fixes.7z",
                          "md5": "11112222333344445555666677778888" } },
            { "name": "Address Library", "version": "1.9.0",
              "source": { "type": "nexus", "modId": 96324, "fileId": 62001,
                          "fileSize": 5242880,
                          "logicalFilename": "address_library.7z",
                          "md5": "4b1dd024876fdddfef2a2383492e1c1c" } },
            { "name": "SSSE", "version": "3.3.3",
              "source": { "type": "nexus", "modId": 60917, "fileId": 71500,
                          "fileSize": 2097152, "logicalFilename": "SSSE_3_3_3.7z",
                          "md5": "add39f916aa4f469b51881fe6b50a9c6" } },
            { "name": "Bejeweled", "version": "1.0",
              "source": { "type": "nexus", "modId": 20301, "fileId": 81000,
                          "fileSize": 1048576, "logicalFilename": "bejeweled.zip",
                          "md5": "e1a03cf9eeb34288cb2d013f61381f63" } }
        ],
        "modRules": [
            { "type": "requires",
              "source": { "fileMD5": "cafebabecafebabecafebabecafebabe" },
              "reference": { "logicalFileName": "skse64_2_02_06.7z" } },
            { "type": "after",
              "source": { "fileSize": 999 },
              "reference": { "fileMD5": "3d1a4b1e2c5f60718293a4b5c6d7e8f90" } },
            { "type": "conflicts",
              "source": { "versionMatch": "1.9.0" },
              "reference": { "versionMatch": "1.4.2" } },
            { "type": "before",
              "source": { "fileExpression": "SSSE_3_3_3" },
              "reference": { "logicalFileName": "SkyUI_1_2_masterfile.7z" } },
            { "type": "recommends",
              "source": { "fileExpression": "Bejeweled" },
              "reference": { "logicalFileName": "address_library.7z" } },
            { "type": "provides",
              "source": { "fileMD5": "4b1dd024876fdddfef2a2383492e1c1c" },
              "reference": { "fileSize": 48213311 } }
        ]
    })");

  REQUIRE(m.rules.size() == 6);
  REQUIRE(m.unresolved.empty());

  REQUIRE(m.rules[0].type == RuleType::Requires);
  REQUIRE(m.rules[0].from == "skyui");
  REQUIRE(m.rules[0].to == "skse64");

  REQUIRE(m.rules[1].type == RuleType::After);
  REQUIRE(m.rules[1].from == "engine-fixes");
  REQUIRE(m.rules[1].to == "skse64");

  REQUIRE(m.rules[2].type == RuleType::Conflicts);
  REQUIRE(m.rules[2].from == "address-library");
  REQUIRE(m.rules[2].to == "skyui");

  REQUIRE(m.rules[3].type == RuleType::Before);
  REQUIRE(m.rules[3].from == "ssse");
  REQUIRE(m.rules[3].to == "skyui");

  REQUIRE(m.rules[4].type == RuleType::Recommends);
  REQUIRE(m.rules[4].from == "bejeweled");
  REQUIRE(m.rules[4].to == "address-library");

  REQUIRE(m.rules[5].type == RuleType::Provides);
  REQUIRE(m.rules[5].from == "address-library");
  REQUIRE(m.rules[5].to == "skyui");
}

TEST_CASE("modRules resolve by the first matching key only", "[collection][nexus]") {
  // A reference carrying two keys must bind on the earlier one. Beta is
  // declared first so a parser that consulted fileExpression first, or that
  // kept looking after the md5 hit, would report the wrong mod.
  const auto m = parse(R"({
        "info": {},
        "mods": [
            { "name": "Beta", "version": "1.0.0",
              "source": { "type": "nexus", "fileSize": 1,
                          "logicalFilename": "beta.zip",
                          "md5": "22222222222222222222222222222222" } },
            { "name": "Alpha", "version": "1.0.0",
              "source": { "type": "nexus", "fileSize": 2,
                          "logicalFilename": "alpha.zip",
                          "md5": "11111111111111111111111111111111" } }
        ],
        "modRules": [
            { "type": "before",
              "source": { "fileMD5": "11111111111111111111111111111111",
                          "fileExpression": "Beta" },
              "reference": { "logicalFileName": "beta.zip" } }
        ]
    })");

  REQUIRE(m.rules.size() == 1);
  REQUIRE(m.rules[0].from == "alpha");
  REQUIRE(m.rules[0].to == "beta");
}

TEST_CASE("modRules versionMatch accepts a caret range", "[collection][nexus]") {
  // New is declared first on purpose: if "^1.0.0" wrongly admitted 2.0.0 the
  // first rule would bind to "new" instead of "old".
  const auto m = parse(R"({
        "info": {},
        "mods": [
            { "name": "New", "version": "2.0.0",
              "source": { "type": "nexus", "md5": "22222222222222222222222222222222" } },
            { "name": "Old", "version": "1.2.3",
              "source": { "type": "nexus", "md5": "11111111111111111111111111111111" } }
        ],
        "modRules": [
            { "type": "before",
              "source": { "versionMatch": "^1.0.0" },
              "reference": { "versionMatch": "^2.0.0" } },
            { "type": "after",
              "source": { "versionMatch": "^2.0.0" },
              "reference": { "versionMatch": "^1.0.0" } }
        ]
    })");

  REQUIRE(m.rules.size() == 2);
  REQUIRE(m.rules[0].from == "old");
  REQUIRE(m.rules[0].to == "new");
  REQUIRE(m.rules[1].from == "new");
  REQUIRE(m.rules[1].to == "old");
}

TEST_CASE("parse modRules defaults to Before for unknown type", "[collection][nexus]") {
  const auto m = parse(R"({
        "info": {},
        "mods": [
            { "name": "Alpha", "source": { "type": "nexus",
                        "logicalFilename": "alpha.zip" } },
            { "name": "Beta", "source": { "type": "nexus",
                        "logicalFilename": "beta.zip" } }
        ],
        "modRules": [
            { "type": "something_weird",
              "source": { "logicalFileName": "alpha.zip" },
              "reference": { "logicalFileName": "beta.zip" } }
        ]
    })");

  REQUIRE(m.rules.size() == 1);
  REQUIRE(m.rules[0].type == RuleType::Before);
  REQUIRE(m.rules[0].from == "alpha");
  REQUIRE(m.rules[0].to == "beta");
}

TEST_CASE("modRules with an unresolvable end is reported, not dropped",
          "[collection][nexus]") {
  // A rule whose ends cannot be bound is surfaced through unresolved with the
  // key that was asked for, so the import never silently loses a rule.
  const auto m = parse(R"({
        "info": {},
        "mods": [
            { "name": "Real Mod", "source": { "type": "nexus",
                        "logicalFilename": "real.zip" } }
        ],
        "modRules": [
            { "type": "before", "source": {},
              "reference": { "logicalFileName": "real.zip" } },
            { "type": "before",
              "source": { "logicalFileName": "real.zip" },
              "reference": { "logicalFileName": "missing.zip" } }
        ]
    })");

  REQUIRE(m.rules.empty());
  REQUIRE(m.unresolved.size() == 2);

  REQUIRE(m.unresolved[0].what == "modRules[0].source");
  REQUIRE(m.unresolved[0].reason.find("empty") != std::string::npos);

  REQUIRE(m.unresolved[1].what == "modRules[1].reference");
  REQUIRE(m.unresolved[1].reason.find("logicalFileName=missing.zip") !=
          std::string::npos);
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
        "mods": [
            { "name": "Alpha", "source": { "type": "nexus",
                        "logicalFilename": "alpha.zip" } },
            { "name": "Beta", "source": { "type": "nexus",
                        "logicalFilename": "beta.zip" } }
        ],
        "modRules": [
            null, 42,
            { "type": "before",
              "source": { "logicalFileName": "alpha.zip" },
              "reference": { "logicalFileName": "beta.zip" } }
        ]
    })");

  REQUIRE(m.rules.size() == 1);
  REQUIRE(m.rules[0].from == "alpha");
  REQUIRE(m.rules[0].to == "beta");
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
    })"),
                    ParseError);
}

TEST_CASE("parse throws on mod missing name", "[collection][nexus]") {
  REQUIRE_THROWS_AS(parse(R"({
        "info": {},
        "mods": [{ "source": { "type": "nexus" } }]
    })"),
                    ParseError);
}

TEST_CASE("parse throws on mod source not an object", "[collection][nexus]") {
  REQUIRE_THROWS_AS(parse(R"({
        "info": {},
        "mods": [{ "name": "Bad Source", "source": "nexus" }]
    })"),
                    ParseError);
}

// ---------------------------------------------------------------------------
// The collection.json documented by Nexus
// ---------------------------------------------------------------------------

TEST_CASE("parse the documented Nexus collection.json", "[collection][nexus]") {
  // Verbatim from the Nexus collections documentation, with one modRule added:
  // the documented example ships an empty modRules array, and a rule is the
  // only way to exercise reference resolution against real mods. The delimiter
  // is named because mod names in the example end in a parenthesis.
  const auto m = parse(R"JSON({
  "info": {
    "author": "Anonymous",
    "authorUrl": "",
    "name": "Halgari's Helper",
    "description": "",
    "installInstructions": "",
    "domainName": "cyberpunk2077",
    "gameVersions": [
      "3.0.76.64179"
    ]
  },
  "mods": [
    {
      "name": "Appearance Menu Mod",
      "version": "2.7",
      "optional": false,
      "domainName": "cyberpunk2077",
      "source": {
        "type": "nexus",
        "modId": 790,
        "fileId": 66386,
        "md5": "0a6e3e603ef3bca799436f69510c79b7",
        "fileSize": 140159937,
        "logicalFilename": "Appearance Menu Mod",
        "updatePolicy": "prefer",
        "tag": "JqF6xzzWA"
      },
      "hashes": [
        {
          "path": "archive\\pc\\mod\\AMM_Dino_TattooFix.archive",
          "md5": "add39f916aa4f469b51881fe6b50a9c6"
        },
        {
          "path": "archive\\pc\\mod\\AMM_RitaWheeler_CombatEnabler.archive",
          "md5": "e1a03cf9eeb34288cb2d013f61381f63"
        }
      ],
      "author": "MaximiliumM and CtrlAltDaz",
      "details": {
        "category": "Appearance",
        "type": ""
      },
      "phase": 0
    },
    {
      "name": "Cyber Engine Tweaks - CET 1.32.2",
      "version": "1.32.2",
      "optional": false,
      "domainName": "cyberpunk2077",
      "source": {
        "type": "nexus",
        "modId": 107,
        "fileId": 73822,
        "md5": "4b1dd024876fdddfef2a2383492e1c1c",
        "fileSize": 34849878,
        "logicalFilename": "CET 1.32.2",
        "updatePolicy": "prefer",
        "tag": "x_A_Q2gQ3e"
      },
      "author": "yamashi",
      "details": {
        "category": "Modders Resources",
        "type": ""
      },
      "phase": 0
    },
    {
      "name": "Load Begone (Intro Splash Load and Checkpoint Removal - FOMOD) - Load Begone - 2.2.1 (FOMOD)",
      "version": "2.2.1",
      "optional": false,
      "domainName": "cyberpunk2077",
      "source": {
        "type": "nexus",
        "modId": 8144,
        "fileId": 59926,
        "md5": "f86b6241862c140891771306282abbf9",
        "fileSize": 3911564,
        "logicalFilename": "Load Begone - 2.2.1 (FOMOD)",
        "updatePolicy": "prefer",
        "tag": "vACbpm9SFd"
      },
      "choices": {
        "type": "fomod",
        "options": [
          {
            "name": "Installation",
            "groups": [
              {
                "name": "Features",
                "choices": [
                  { "name": "Skip Intro Logos", "idx": 0 },
                  { "name": "No Splash Video", "idx": 1 },
                  { "name": "Faster Checkpoints", "idx": 2 }
                ]
              }
            ]
          }
        ]
      },
      "author": "CyanideX",
      "details": {
        "category": "User Interface",
        "type": ""
      },
      "phase": 0
    }
  ],
  "modRules": [
    { "type": "before",
      "source": { "fileMD5": "0a6e3e603ef3bca799436f69510c79b7" },
      "reference": { "logicalFileName": "CET 1.32.2" } }
  ],
  "loadOrder": [],
  "tools": [],
  "collectionConfig": {
    "recommendNewProfile": false
  }
}
)JSON");

  // Info
  REQUIRE(m.info.name == "Halgari's Helper");
  REQUIRE(m.info.author == "Anonymous");
  REQUIRE(m.info.game_id == "cyberpunk2077");
  REQUIRE(m.info.homepage.empty());

  // Mods
  REQUIRE(m.mods.size() == 3);
  REQUIRE(m.unresolved.empty());

  // Mod 0: Appearance Menu Mod. updatePolicy "prefer", and the md5 lands in the
  // md5 field only, never in sha256.
  REQUIRE(m.mods[0].id == "appearance-menu-mod");
  REQUIRE(m.mods[0].phase == 0);
  REQUIRE(m.mods[0].category == ModCategory::Required);
  REQUIRE(std::holds_alternative<SourceNexus>(m.mods[0].source));
  const auto &amm = std::get<SourceNexus>(m.mods[0].source);
  REQUIRE(amm.mod_id == 790);
  REQUIRE(amm.file_id == 66386);
  REQUIRE(amm.file_size == 140159937);
  REQUIRE(amm.file_name == "Appearance Menu Mod");
  REQUIRE(amm.md5 == "0a6e3e603ef3bca799436f69510c79b7");
  REQUIRE(amm.sha256.empty());
  REQUIRE(amm.update_policy == UpdatePolicy::Prefer);

  // Mod 1: Cyber Engine Tweaks
  REQUIRE(m.mods[1].id == "cyber-engine-tweaks-cet-1-32-2");
  REQUIRE(std::holds_alternative<SourceNexus>(m.mods[1].source));
  const auto &cet = std::get<SourceNexus>(m.mods[1].source);
  REQUIRE(cet.mod_id == 107);
  REQUIRE(cet.file_id == 73822);
  REQUIRE(cet.md5 == "4b1dd024876fdddfef2a2383492e1c1c");
  REQUIRE(cet.file_name == "CET 1.32.2");

  // Mod 2: Load Begone, a FOMOD mod with nested choices
  REQUIRE(m.mods[2].name ==
          "Load Begone (Intro Splash Load and Checkpoint Removal - FOMOD) - "
          "Load Begone - 2.2.1 (FOMOD)");
  REQUIRE(m.mods[2].installer_choices.type == "fomod");

  // The rule binds through source.fileMD5 and reference.logicalFileName.
  REQUIRE(m.rules.size() == 1);
  REQUIRE(m.rules[0].type == RuleType::Before);
  REQUIRE(m.rules[0].from == "appearance-menu-mod");
  REQUIRE(m.rules[0].to == "cyber-engine-tweaks-cet-1-32-2");
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
  REQUIRE_FALSE(nx.game_domain.empty() == false);  // empty by default
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
