// Tests for the thin INI tweak parser layer (ini_edit_parser.h/cpp).
//
// Covers JSON -> IniTweak parsing, conversion to the shared engine structs,
// delegation to modpack/ini_edits for merge/apply, and referential integrity.

#include "engine/gmmpack/ini_edit_parser.h"

#include "engine/gmmpack/unpacker.h"
#include "engine/modpack/ini_edits.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace gmmpack = engine::gmmpack;
namespace modpack = engine::modpack;

// ---------------------------------------------------------------------------
// parse_ini_entry
// ---------------------------------------------------------------------------

TEST_CASE("ini_edit_parser parse_ini_entry with null sourceModId",
          "[ini_edit_parser]") {
  nlohmann::json j;
  j["targetFile"] = "Skyrim.ini";
  j["tweaks"]     = {{{"id", "aniso"},
                      {"name", "Anisotropy"},
                      {"status", "required"},
                      {"enabled", true},
                      {"sourceModId", nullptr},
                      {"content", "[Display]\niMaxAnisotropy=16\n"}}};

  auto entry = gmmpack::parse_ini_entry(j);
  REQUIRE(entry.target_file == "Skyrim.ini");
  REQUIRE(entry.tweaks.size() == 1);
  CHECK(entry.tweaks[0].id == "aniso");
  CHECK(entry.tweaks[0].name == "Anisotropy");
  CHECK(entry.tweaks[0].status == "required");
  CHECK(entry.tweaks[0].enabled);
  CHECK(entry.tweaks[0].content == "[Display]\niMaxAnisotropy=16\n");
  REQUIRE_FALSE(entry.tweaks[0].has_source_mod_id);
  REQUIRE(entry.tweaks[0].source_mod_id.empty());
}

TEST_CASE("ini_edit_parser parse_ini_entry with string sourceModId",
          "[ini_edit_parser]") {
  nlohmann::json j;
  j["targetFile"] = "Skyrim.ini";
  j["tweaks"]     = {{{"id", "skyui-list"},
                      {"name", "SkyUI list"},
                      {"status", "recommended"},
                      {"enabled", false},
                      {"sourceModId", "skyui"},
                      {"content", "[Archive]\nsResourceArchiveList2=...\n"}}};

  auto entry = gmmpack::parse_ini_entry(j);
  REQUIRE(entry.target_file == "Skyrim.ini");
  REQUIRE(entry.tweaks.size() == 1);
  REQUIRE(entry.tweaks[0].has_source_mod_id);
  REQUIRE(entry.tweaks[0].source_mod_id == "skyui");
  REQUIRE_FALSE(entry.tweaks[0].enabled);
}

TEST_CASE("ini_edit_parser parse_ini_entry with empty tweaks array",
          "[ini_edit_parser]") {
  nlohmann::json j;
  j["targetFile"] = "Skyrim.ini";
  j["tweaks"]     = nlohmann::json::array();

  auto entry = gmmpack::parse_ini_entry(j);
  REQUIRE(entry.target_file == "Skyrim.ini");
  REQUIRE(entry.tweaks.empty());
}

TEST_CASE("ini_edit_parser parse_ini_entry multiple tweaks", "[ini_edit_parser]") {
  nlohmann::json j;
  j["targetFile"] = "SkyrimPrefs.ini";
  j["tweaks"]     = {
      {{"id", "shadows"},
       {"name", "Shadows"},
       {"status", "required"},
       {"enabled", true},
       {"sourceModId", nullptr},
       {"content", "[Display]\niShadowMapResolution=4096\n"}},
      {{"id", "grass"},
       {"name", "Grass"},
       {"status", "recommended"},
       {"enabled", true},
       {"sourceModId", "nature-mod"},
       {"content", "[Grass]\niGrassDensity=128\n"}},
  };

  auto entry = gmmpack::parse_ini_entry(j);
  REQUIRE(entry.target_file == "SkyrimPrefs.ini");
  REQUIRE(entry.tweaks.size() == 2);
  REQUIRE_FALSE(entry.tweaks[0].has_source_mod_id);
  REQUIRE(entry.tweaks[1].has_source_mod_id);
  REQUIRE(entry.tweaks[1].source_mod_id == "nature-mod");
}

// ---------------------------------------------------------------------------
// to_edit_file conversion
// ---------------------------------------------------------------------------

TEST_CASE("ini_edit_parser to_edit_file maps status and source", "[ini_edit_parser]") {
  gmmpack::IniEntry entry;
  entry.target_file = "Skyrim.ini";
  gmmpack::IniTweak req;
  req.id      = "aniso";
  req.name    = "Anisotropy";
  req.status  = "required";
  req.enabled = true;
  req.content = "[Display]\niMaxAnisotropy=16\n";
  gmmpack::IniTweak rec;
  rec.id                = "skyui-list";
  rec.name              = "SkyUI list";
  rec.status            = "recommended";
  rec.enabled           = false;
  rec.content           = "[Archive]\nsResourceArchiveList2=...\n";
  rec.source_mod_id     = "skyui";
  rec.has_source_mod_id = true;
  entry.tweaks          = {req, rec};

  auto file = gmmpack::to_edit_file(entry);
  REQUIRE(file.target_file == "Skyrim.ini");
  REQUIRE(file.tweaks.size() == 2);
  CHECK(file.tweaks[0].status == modpack::TweakStatus::Required);
  CHECK(file.tweaks[0].enabled);
  CHECK(!file.tweaks[0].source_mod_id.has_value());
  CHECK(file.tweaks[1].status == modpack::TweakStatus::Recommended);
  CHECK(!file.tweaks[1].enabled);
  REQUIRE(file.tweaks[1].source_mod_id.has_value());
  CHECK(*file.tweaks[1].source_mod_id == "skyui");
}

TEST_CASE("ini_edit_parser to_edit_file defaults unknown status", "[ini_edit_parser]") {
  gmmpack::IniEntry entry;
  entry.target_file = "x.ini";
  gmmpack::IniTweak tweak;
  tweak.id      = "t";
  tweak.name    = "T";
  tweak.status  = "bogus";  // schema-validated input never has this
  tweak.content = "[S]\nk=v\n";
  entry.tweaks  = {tweak};

  auto file = gmmpack::to_edit_file(entry);
  REQUIRE(file.tweaks.size() == 1);
  CHECK(file.tweaks[0].status == modpack::TweakStatus::Recommended);
}

// ---------------------------------------------------------------------------
// Delegation: gmmpack parse -> engine merge/apply
// ---------------------------------------------------------------------------

TEST_CASE("ini_edit_parser delegates merge and apply to the engine",
          "[ini_edit_parser]") {
  nlohmann::json j;
  j["targetFile"] = "Skyrim.ini";
  j["tweaks"]     = {
      {{"id", "aniso"},
       {"name", "Anisotropy"},
       {"status", "required"},
       {"enabled", true},
       {"sourceModId", nullptr},
       {"content", "[Display]\niMaxAnisotropy=16\n"}},
      {{"id", "off"},
       {"name", "Disabled"},
       {"status", "recommended"},
       {"enabled", false},
       {"sourceModId", nullptr},
       {"content", "[Display]\nbFull Screen=0\n"}},
  };

  auto file   = gmmpack::to_edit_file(gmmpack::parse_ini_entry(j));
  auto merged = modpack::merge_ini_edits({file});
  REQUIRE(merged.size() == 1);
  REQUIRE(merged[0].edits.size() == 1);  // disabled tweak filtered
  CHECK(merged[0].edits[0].tweak_id == std::string("aniso"));

  const std::string ini = "[Display]\niMaxAnisotropy=4\nbFull Screen=1\n";
  auto out              = modpack::apply_ini_edits(ini, merged[0]);
  CHECK(out.text.find("iMaxAnisotropy=16\n") != std::string::npos);
  CHECK(out.text.find("bFull Screen=1\n") != std::string::npos);
  REQUIRE(out.applied.size() == 1);
  CHECK(out.applied[0].tweak_id == std::string("aniso"));
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
  gmmpack::IniTweak tweak;
  tweak.id                = "bad";
  tweak.name              = "Bad";
  tweak.status            = "recommended";
  tweak.enabled           = true;
  tweak.content           = "[Display]\niMaxAnisotropy=16\n";
  tweak.source_mod_id     = "nonexistent-mod";
  tweak.has_source_mod_id = true;
  ini.tweaks.push_back(tweak);
  pack.ini_edits.push_back(ini);

  auto diag = gmmpack::check_referential_integrity(pack);
  REQUIRE_FALSE(diag.empty());
  bool found = false;
  for (const auto &d : diag) {
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
  gmmpack::IniTweak pack_tweak;
  pack_tweak.id      = "aniso";
  pack_tweak.name    = "Anisotropy";
  pack_tweak.status  = "required";
  pack_tweak.enabled = true;
  pack_tweak.content = "[Display]\niMaxAnisotropy=16\n";
  ini.tweaks.push_back(pack_tweak);
  gmmpack::IniTweak mod_tweak;
  mod_tweak.id                = "skyui-list";
  mod_tweak.name              = "SkyUI list";
  mod_tweak.status            = "recommended";
  mod_tweak.enabled           = true;
  mod_tweak.content           = "[Archive]\nsResourceArchiveList2=a.esp\n";
  mod_tweak.source_mod_id     = "skyui";
  mod_tweak.has_source_mod_id = true;
  ini.tweaks.push_back(mod_tweak);
  pack.ini_edits.push_back(ini);

  auto diag = gmmpack::check_referential_integrity(pack);
  REQUIRE(diag.empty());
}

TEST_CASE("ini_edit_parser ref integrity catches duplicate tweak ids",
          "[ini_edit_parser]") {
  gmmpack::Gmmpack pack;
  gmmpack::IniEntry ini;
  ini.target_file = "Skyrim.ini";
  for (int i = 0; i < 2; ++i) {
    gmmpack::IniTweak tweak;
    tweak.id      = "same-id";
    tweak.name    = "Name " + std::to_string(i);
    tweak.status  = "required";
    tweak.enabled = true;
    tweak.content = "[Display]\niMaxAnisotropy=16\n";
    ini.tweaks.push_back(tweak);
  }
  pack.ini_edits.push_back(ini);

  auto diag = gmmpack::check_referential_integrity(pack);
  REQUIRE_FALSE(diag.empty());
  bool found = false;
  for (const auto &d : diag) {
    if (d.message.find("duplicate tweak id") != std::string::npos) {
      found = true;
      break;
    }
  }
  REQUIRE(found);
}
