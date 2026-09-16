// Tests for the INI edit parser consolidation layer (ini_edit_parser.h/cpp).
//
// Covers consolidation, retraction (update-algorithm step 5), duplicate
// detection, and collector helpers.

#include "engine/gmmpack/ini_edit_parser.h"
#include "engine/gmmpack/unpacker.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace gmmpack = engine::gmmpack;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static gmmpack::IniEdit make_edit(const std::string& section,
                                   const std::string& key,
                                   const std::string& value,
                                   const std::string& source_mod_id = "",
                                   bool has_source = false) {
    gmmpack::IniEdit e;
    e.section = section;
    e.key = key;
    e.value = value;
    e.source_mod_id = source_mod_id;
    e.has_source_mod_id = has_source;
    return e;
}

static gmmpack::IniEntry make_entry(
    const std::string& target,
    const std::vector<gmmpack::IniEdit>& edits) {
    gmmpack::IniEntry ie;
    ie.target_file = target;
    ie.edits = edits;
    return ie;
}

// ---------------------------------------------------------------------------
// Consolidation
// ---------------------------------------------------------------------------

TEST_CASE("ini_edit_parser consolidate empty input", "[ini_edit_parser]") {
    std::vector<gmmpack::IniEntry> entries;
    auto con = gmmpack::consolidate_ini_edits(entries);
    REQUIRE(con.by_target.empty());
    REQUIRE(con.by_source.empty());
}

TEST_CASE("ini_edit_parser consolidate single target", "[ini_edit_parser]") {
    gmmpack::IniEntry entry = make_entry("Skyrim.ini", {
        make_edit("Display", "iMaxAnisotropy", "16"),
        make_edit("Display", "iShadowMapResolution", "4096"),
    });
    auto con = gmmpack::consolidate_ini_edits({entry});

    REQUIRE(con.by_target.size() == 1);
    REQUIRE(con.by_target.count("Skyrim.ini"));
    REQUIRE(con.by_target.at("Skyrim.ini").edits.size() == 2);

    // Pack-author edits grouped under empty string
    REQUIRE(con.by_source.count("") == 1);
    REQUIRE(con.by_source.at("").size() == 2);
}

TEST_CASE("ini_edit_parser consolidate multiple targets", "[ini_edit_parser]") {
    gmmpack::IniEntry e1 = make_entry("Skyrim.ini", {
        make_edit("Display", "iMaxAnisotropy", "16"),
    });
    gmmpack::IniEntry e2 = make_entry("SkyrimPrefs.ini", {
        make_edit("Grass", "iGrassDensity", "128"),
    });
    auto con = gmmpack::consolidate_ini_edits({e1, e2});

    REQUIRE(con.by_target.size() == 2);
    REQUIRE(con.by_target.count("Skyrim.ini"));
    REQUIRE(con.by_target.count("SkyrimPrefs.ini"));
}

TEST_CASE("ini_edit_parser consolidate tracks source attribution",
          "[ini_edit_parser]") {
    gmmpack::IniEntry entry = make_entry("Skyrim.ini", {
        make_edit("Display", "iMaxAnisotropy", "16"),             // pack-author
        make_edit("Archive", "sResourceArchiveList2", "...", "skyui", true),  // mod
    });
    auto con = gmmpack::consolidate_ini_edits({entry});

    // Two source groups: "" (pack-author) and "skyui"
    REQUIRE(con.by_source.size() == 2);
    REQUIRE(con.by_source.count("") == 1);
    REQUIRE(con.by_source.at("").size() == 1);
    REQUIRE(con.by_source.count("skyui") == 1);
    REQUIRE(con.by_source.at("skyui").size() == 1);
}

TEST_CASE("ini_edit_parser consolidate merges duplicate targets",
          "[ini_edit_parser]") {
    // Two entries for same target file (consolidated from multiple ini/*.json
    // that somehow both target the same file)
    gmmpack::IniEntry e1 = make_entry("Skyrim.ini", {
        make_edit("Display", "iMaxAnisotropy", "16"),
    });
    gmmpack::IniEntry e2 = make_entry("Skyrim.ini", {
        make_edit("Display", "bFull Screen", "1"),
    });
    auto con = gmmpack::consolidate_ini_edits({e1, e2});

    REQUIRE(con.by_target.size() == 1);
    REQUIRE(con.by_target.at("Skyrim.ini").edits.size() == 2);
}

// ---------------------------------------------------------------------------
// Retraction
// ---------------------------------------------------------------------------

TEST_CASE("ini_edit_parser retract_mod_edits removes mod's edits",
          "[ini_edit_parser]") {
    gmmpack::IniEntry entry = make_entry("Skyrim.ini", {
        make_edit("Display", "iMaxAnisotropy", "16"),                   // pack
        make_edit("Archive", "sResourceArchiveList2", "...", "skyui", true),  // mod
    });
    auto con = gmmpack::consolidate_ini_edits({entry});

    auto retracted = gmmpack::retract_mod_edits(con, "skyui");

    REQUIRE(retracted.size() == 1);
    REQUIRE(retracted[0].section == "Archive");
    REQUIRE(retracted[0].key == "sResourceArchiveList2");
    REQUIRE(con.by_target.at("Skyrim.ini").edits.size() == 1);
    REQUIRE(con.by_target.at("Skyrim.ini").edits[0].section == "Display");
}

TEST_CASE("ini_edit_parser retract_mod_edits no-op when mod not present",
          "[ini_edit_parser]") {
    gmmpack::IniEntry entry = make_entry("Skyrim.ini", {
        make_edit("Display", "iMaxAnisotropy", "16"),
    });
    auto con = gmmpack::consolidate_ini_edits({entry});

    auto retracted = gmmpack::retract_mod_edits(con, "nonexistent");

    REQUIRE(retracted.empty());
    REQUIRE(con.by_target.at("Skyrim.ini").edits.size() == 1);
}

TEST_CASE("ini_edit_parser retract_pack_author_edits with empty mod_id",
          "[ini_edit_parser]") {
    gmmpack::IniEntry entry = make_entry("Skyrim.ini", {
        make_edit("Display", "iMaxAnisotropy", "16"),                   // pack
        make_edit("Archive", "sResourceArchiveList2", "...", "skyui", true),
    });
    auto con = gmmpack::consolidate_ini_edits({entry});

    auto retracted = gmmpack::retract_mod_edits(con, "");

    REQUIRE(retracted.size() == 1);
    REQUIRE(retracted[0].section == "Display");
    REQUIRE(con.by_target.at("Skyrim.ini").edits.size() == 1);
}

TEST_CASE("ini_edit_parser retract_mod_edits across multiple targets",
          "[ini_edit_parser]") {
    gmmpack::IniEntry e1 = make_entry("Skyrim.ini", {
        make_edit("Display", "iMaxAnisotropy", "16"),
        make_edit("Archive", "sResourceArchiveList2", "...", "skyui", true),
    });
    gmmpack::IniEntry e2 = make_entry("SkyrimPrefs.ini", {
        make_edit("Grass", "iGrassDensity", "128", "skyui", true),
        make_edit("Water", "bReflectSky", "1"),
    });
    auto con = gmmpack::consolidate_ini_edits({e1, e2});

    auto retracted = gmmpack::retract_mod_edits(con, "skyui");

    REQUIRE(retracted.size() == 2);
    REQUIRE(con.by_target.at("Skyrim.ini").edits.size() == 1);
    REQUIRE(con.by_target.at("SkyrimPrefs.ini").edits.size() == 1);
}

TEST_CASE("ini_edit_parser retract rebuilds by_source index",
          "[ini_edit_parser]") {
    gmmpack::IniEntry entry = make_entry("Skyrim.ini", {
        make_edit("Display", "iMaxAnisotropy", "16"),
        make_edit("Archive", "sResourceArchiveList2", "...", "skyui", true),
    });
    auto con = gmmpack::consolidate_ini_edits({entry});

    gmmpack::retract_mod_edits(con, "skyui");

    // by_source should no longer contain "skyui"
    REQUIRE_FALSE(con.by_source.count("skyui"));
    // Only pack-author remains
    REQUIRE(con.by_source.size() == 1);
    REQUIRE(con.by_source.count(""));
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

TEST_CASE("ini_edit_parser validate passes on clean edits", "[ini_edit_parser]") {
    gmmpack::IniEntry entry = make_entry("Skyrim.ini", {
        make_edit("Display", "iMaxAnisotropy", "16"),
        make_edit("Display", "iShadowMapResolution", "4096"),
    });
    auto con = gmmpack::consolidate_ini_edits({entry});

    auto diag = gmmpack::validate_consolidated_edits(con);
    REQUIRE(diag.empty());
}

TEST_CASE("ini_edit_parser validate detects empty section", "[ini_edit_parser]") {
    gmmpack::IniEntry entry = make_entry("Skyrim.ini", {
        make_edit("", "iMaxAnisotropy", "16"),
    });
    auto con = gmmpack::consolidate_ini_edits({entry});

    auto diag = gmmpack::validate_consolidated_edits(con);
    REQUIRE_FALSE(diag.empty());
    bool found = false;
    for (const auto& d : diag) {
        if (d.message.find("empty section") != std::string::npos) {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ini_edit_parser validate detects empty key", "[ini_edit_parser]") {
    gmmpack::IniEntry entry = make_entry("Skyrim.ini", {
        make_edit("Display", "", "16"),
    });
    auto con = gmmpack::consolidate_ini_edits({entry});

    auto diag = gmmpack::validate_consolidated_edits(con);
    REQUIRE_FALSE(diag.empty());
    bool found = false;
    for (const auto& d : diag) {
        if (d.message.find("empty key") != std::string::npos) {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ini_edit_parser validate detects empty value", "[ini_edit_parser]") {
    gmmpack::IniEntry entry = make_entry("Skyrim.ini", {
        make_edit("Display", "iMaxAnisotropy", ""),
    });
    auto con = gmmpack::consolidate_ini_edits({entry});

    auto diag = gmmpack::validate_consolidated_edits(con);
    REQUIRE_FALSE(diag.empty());
    bool found = false;
    for (const auto& d : diag) {
        if (d.message.find("empty value") != std::string::npos) {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ini_edit_parser validate warns on duplicate section/key different value",
          "[ini_edit_parser]") {
    gmmpack::IniEntry entry = make_entry("Skyrim.ini", {
        make_edit("Display", "iMaxAnisotropy", "16", "mod-a", true),
        make_edit("Display", "iMaxAnisotropy", "32", "mod-b", true),
    });
    auto con = gmmpack::consolidate_ini_edits({entry});

    auto diag = gmmpack::validate_consolidated_edits(con);
    REQUIRE_FALSE(diag.empty());
    bool found_warning = false;
    for (const auto& d : diag) {
        if (d.severity == gmmpack::Diagnostic::Severity::Warning &&
            d.message.find("duplicate") != std::string::npos) {
            found_warning = true;
            break;
        }
    }
    REQUIRE(found_warning);
}

TEST_CASE("ini_edit_parser validate no warning for same section/key same value",
          "[ini_edit_parser]") {
    gmmpack::IniEntry entry = make_entry("Skyrim.ini", {
        make_edit("Display", "iMaxAnisotropy", "16", "mod-a", true),
        make_edit("Display", "iMaxAnisotropy", "16", "mod-b", true),
    });
    auto con = gmmpack::consolidate_ini_edits({entry});

    auto diag = gmmpack::validate_consolidated_edits(con);
    REQUIRE(diag.empty());
}

// ---------------------------------------------------------------------------
// Collectors
// ---------------------------------------------------------------------------

TEST_CASE("ini_edit_parser collect_sections", "[ini_edit_parser]") {
    gmmpack::IniEntry entry = make_entry("Skyrim.ini", {
        make_edit("Display", "iMaxAnisotropy", "16"),
        make_edit("Display", "iShadowMapResolution", "4096"),
        make_edit("Archive", "sResourceArchiveList2", "..."),
    });
    auto con = gmmpack::consolidate_ini_edits({entry});

    auto sections = gmmpack::collect_sections(con);
    REQUIRE(sections.size() == 2);
    std::sort(sections.begin(), sections.end());
    REQUIRE(sections[0] == "Archive");
    REQUIRE(sections[1] == "Display");
}

TEST_CASE("ini_edit_parser collect_sources", "[ini_edit_parser]") {
    gmmpack::IniEntry entry = make_entry("Skyrim.ini", {
        make_edit("Display", "iMaxAnisotropy", "16"),                   // pack
        make_edit("Archive", "sResourceArchiveList2", "...", "skyui", true),
    });
    auto con = gmmpack::consolidate_ini_edits({entry});

    auto sources = gmmpack::collect_sources(con);
    REQUIRE(sources.size() == 2);
    std::sort(sources.begin(), sources.end());
    REQUIRE(sources[0] == "");
    REQUIRE(sources[1] == "skyui");
}

// ---------------------------------------------------------------------------
// parse_ini_entry (in unpacker.cpp, but exercises the round-trip)
// ---------------------------------------------------------------------------

TEST_CASE("ini_edit_parser parse_ini_entry with null sourceModId",
          "[ini_edit_parser]") {
    nlohmann::json j;
    j["targetFile"] = "Skyrim.ini";
    j["edits"] = {{{"section", "Display"},
                    {"key", "iMaxAnisotropy"},
                    {"value", "16"},
                    {"sourceModId", nullptr}}};

    auto entry = gmmpack::parse_ini_entry(j);
    REQUIRE(entry.target_file == "Skyrim.ini");
    REQUIRE(entry.edits.size() == 1);
    REQUIRE_FALSE(entry.edits[0].has_source_mod_id);
    REQUIRE(entry.edits[0].source_mod_id.empty());
}

TEST_CASE("ini_edit_parser parse_ini_entry with string sourceModId",
          "[ini_edit_parser]") {
    nlohmann::json j;
    j["targetFile"] = "Skyrim.ini";
    j["edits"] = {{{"section", "Archive"},
                    {"key", "sResourceArchiveList2"},
                    {"value", "..."},
                    {"sourceModId", "skyui"}}};

    auto entry = gmmpack::parse_ini_entry(j);
    REQUIRE(entry.target_file == "Skyrim.ini");
    REQUIRE(entry.edits.size() == 1);
    REQUIRE(entry.edits[0].has_source_mod_id);
    REQUIRE(entry.edits[0].source_mod_id == "skyui");
}

TEST_CASE("ini_edit_parser parse_ini_entry with empty edits array",
          "[ini_edit_parser]") {
    nlohmann::json j;
    j["targetFile"] = "Skyrim.ini";
    j["edits"] = nlohmann::json::array();

    auto entry = gmmpack::parse_ini_entry(j);
    REQUIRE(entry.target_file == "Skyrim.ini");
    REQUIRE(entry.edits.empty());
}

TEST_CASE("ini_edit_parser parse_ini_entry multiple edits",
          "[ini_edit_parser]") {
    nlohmann::json j;
    j["targetFile"] = "SkyrimPrefs.ini";
    j["edits"] = {
        {{"section", "Display"}, {"key", "iShadowMapResolution"}, {"value", "4096"}, {"sourceModId", nullptr}},
        {{"section", "Grass"}, {"key", "iGrassDensity"}, {"value", "128"}, {"sourceModId", "nature-mod"}},
        {{"section", "Water"}, {"key", "bReflectSky"}, {"value", "1"}, {"sourceModId", nullptr}},
    };

    auto entry = gmmpack::parse_ini_entry(j);
    REQUIRE(entry.target_file == "SkyrimPrefs.ini");
    REQUIRE(entry.edits.size() == 3);

    REQUIRE_FALSE(entry.edits[0].has_source_mod_id);
    REQUIRE(entry.edits[1].has_source_mod_id);
    REQUIRE(entry.edits[1].source_mod_id == "nature-mod");
    REQUIRE_FALSE(entry.edits[2].has_source_mod_id);
}

// ---------------------------------------------------------------------------
// End-to-end: parse -> consolidate -> retract -> validate
// ---------------------------------------------------------------------------

TEST_CASE("ini_edit_parser full pipeline", "[ini_edit_parser]") {
    // Simulate a pack with two ini files and multiple source mods
    nlohmann::json j1;
    j1["targetFile"] = "Skyrim.ini";
    j1["edits"] = {
        {{"section", "Display"}, {"key", "iMaxAnisotropy"}, {"value", "16"}, {"sourceModId", nullptr}},
        {{"section", "Archive"}, {"key", "sResourceArchiveList2"}, {"value", "a.esp,b.esp"}, {"sourceModId", "skyui"}},
    };
    nlohmann::json j2;
    j2["targetFile"] = "SkyrimPrefs.ini";
    j2["edits"] = {
        {{"section", "Display"}, {"key", "iShadowMapResolution"}, {"value", "4096"}, {"sourceModId", "shadows-mod"}},
        {{"section", "Grass"}, {"key", "iGrassDensity"}, {"value", "128"}, {"sourceModId", nullptr}},
    };

    auto e1 = gmmpack::parse_ini_entry(j1);
    auto e2 = gmmpack::parse_ini_entry(j2);

    // Consolidate
    auto con = gmmpack::consolidate_ini_edits({e1, e2});
    REQUIRE(con.by_target.size() == 2);
    REQUIRE(con.by_source.size() == 3);  // "", "skyui", "shadows-mod"

    // Retract skyui (mod removed)
    auto retracted = gmmpack::retract_mod_edits(con, "skyui");
    REQUIRE(retracted.size() == 1);
    REQUIRE(retracted[0].key == "sResourceArchiveList2");
    REQUIRE(con.by_target.at("Skyrim.ini").edits.size() == 1);

    // Retract shadows-mod
    retracted = gmmpack::retract_mod_edits(con, "shadows-mod");
    REQUIRE(retracted.size() == 1);
    REQUIRE(con.by_target.at("SkyrimPrefs.ini").edits.size() == 1);

    // Validate remaining - no warnings or errors
    auto diag = gmmpack::validate_consolidated_edits(con);
    REQUIRE(diag.empty());

    // Verify remaining edits are the pack-author ones
    auto sections = gmmpack::collect_sections(con);
    REQUIRE(sections.size() == 2);
    auto sources = gmmpack::collect_sources(con);
    REQUIRE(sources.size() == 1);
    REQUIRE(sources[0] == "");  // only pack-author
}

// ---------------------------------------------------------------------------
// Referential integrity: ini sourceModId must resolve to a real mod id
// ---------------------------------------------------------------------------

TEST_CASE("ini_edit_parser ref integrity catches dangling sourceModId",
          "[ini_edit_parser]") {
    gmmpack::Gmmpack pack;
    gmmpack::ModEntry mod;
    mod.id = "skyui";
    pack.mods.push_back(mod);

    gmmpack::IniEntry ini;
    ini.target_file = "Skyrim.ini";
    gmmpack::IniEdit edit;
    edit.section = "Display";
    edit.key = "iMaxAnisotropy";
    edit.value = "16";
    edit.source_mod_id = "nonexistent-mod";
    edit.has_source_mod_id = true;
    ini.edits.push_back(edit);
    pack.ini_edits.push_back(ini);

    auto diag = gmmpack::check_referential_integrity(pack);
    REQUIRE_FALSE(diag.empty());
    bool found = false;
    for (const auto& d : diag) {
        if (d.message.find("unknown mod id") != std::string::npos) {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ini_edit_parser ref integrity passes for valid attribution",
          "[ini_edit_parser]") {
    gmmpack::Gmmpack pack;
    gmmpack::ModEntry mod;
    mod.id = "skyui";
    pack.mods.push_back(mod);

    gmmpack::IniEntry ini;
    ini.target_file = "Skyrim.ini";
    gmmpack::IniEdit pack_edit;
    pack_edit.section = "Display";
    pack_edit.key = "iMaxAnisotropy";
    pack_edit.value = "16";
    ini.edits.push_back(pack_edit);
    gmmpack::IniEdit mod_edit;
    mod_edit.section = "Archive";
    mod_edit.key = "sResourceArchiveList2";
    mod_edit.value = "a.esp";
    mod_edit.source_mod_id = "skyui";
    mod_edit.has_source_mod_id = true;
    ini.edits.push_back(mod_edit);
    pack.ini_edits.push_back(ini);

    auto diag = gmmpack::check_referential_integrity(pack);
    REQUIRE(diag.empty());
}
