// Tests for the mods/<id>.json parser (parse_mod_entry).
//
// Covers:
//   - All 5 source providers (Nexus, LoversLab, ModPub, Steam Workshop, Direct)
//   - updatePolicy variants (exact vs latest) and conditional required fields
//   - Category enum parsing (required / optional / recommended)
//   - Phase field
//   - InstallerChoices (type + selections)
//   - Minimal vs full JSON payloads
//   - Edge cases (unknown provider, missing optional fields, variant modId types)

#include "engine/gmmpack/unpacker.h"

#include <catch2/catch_test_macros.hpp>

namespace gmmpack = engine::gmmpack;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static nlohmann::json mod_json(const std::string &id = "skyui") {
  nlohmann::json j;
  j["id"]       = id;
  j["name"]     = "SkyUI";
  j["category"] = "required";
  j["source"]   = {{"provider", "nexus"},
                   {"resolution", "api"},
                   {"gameDomain", "skyrimspecialedition"},
                   {"modId", 12345},
                   {"fileId", 67890},
                   {"version", "1.4.2"},
                   {"fileName", "SkyUI-1.4.2.7z"},
                   {"fileSize", 48213311},
                   {"sha256", "abcdef0123456789abcdef0123456789abcdef0123456789abcdef01"
                              "23456789"},
                   {"updatePolicy", "exact"}};
  return j;
}

// ---------------------------------------------------------------------------
// Nexus provider
// ---------------------------------------------------------------------------

TEST_CASE("parse_mod_entry: Nexus exact", "[gmmpack][mod_entry]") {
  auto j = mod_json();
  auto m = gmmpack::parse_mod_entry(j);

  REQUIRE(m.id == "skyui");
  REQUIRE(m.name == "SkyUI");
  REQUIRE(m.category == gmmpack::ModCategory::Required);
  REQUIRE(m.phase == 0);

  auto *src = std::get_if<gmmpack::ModSourceNexus>(&m.source);
  REQUIRE(src != nullptr);
  REQUIRE(src->provider == "nexus");
  REQUIRE(src->resolution == "api");
  REQUIRE(src->game_domain == "skyrimspecialedition");
  REQUIRE(src->mod_id == 12345);
  REQUIRE(src->file_id.has_value());
  REQUIRE(*src->file_id == 67890);
  REQUIRE(src->version.has_value());
  REQUIRE(*src->version == "1.4.2");
  REQUIRE(src->file_name.has_value());
  REQUIRE(*src->file_name == "SkyUI-1.4.2.7z");
  REQUIRE(src->file_size.has_value());
  REQUIRE(*src->file_size == 48213311);
  REQUIRE(src->sha256.has_value());
  REQUIRE(src->update_policy == "exact");
}

TEST_CASE("parse_mod_entry: Nexus latest (no fileId/version/hash)",
          "[gmmpack][mod_entry]") {
  auto j      = mod_json();
  j["source"] = {{"provider", "nexus"},
                 {"resolution", "browser"},
                 {"gameDomain", "skyrimspecialedition"},
                 {"modId", 99999},
                 {"updatePolicy", "latest"}};
  auto m      = gmmpack::parse_mod_entry(j);

  auto *src = std::get_if<gmmpack::ModSourceNexus>(&m.source);
  REQUIRE(src != nullptr);
  REQUIRE(src->mod_id == 99999);
  REQUIRE(src->update_policy == "latest");
  REQUIRE_FALSE(src->file_id.has_value());
  REQUIRE_FALSE(src->version.has_value());
  REQUIRE_FALSE(src->sha256.has_value());
}

// ---------------------------------------------------------------------------
// LoversLab provider
// ---------------------------------------------------------------------------

TEST_CASE("parse_mod_entry: LoversLab with int modId", "[gmmpack][mod_entry]") {
  auto j      = mod_json();
  j["source"] = {{"provider", "loverslab"},
                 {"resolution", "browser"},
                 {"modId", 42},
                 {"sectionSlug", "animation"},
                 {"version", "2.0"},
                 {"updatePolicy", "exact"},
                 {"sha256", "abcdef0123456789abcdef0123456789abcdef0123456789abcdef01"
                            "23456789"}};
  auto m      = gmmpack::parse_mod_entry(j);

  auto *src = std::get_if<gmmpack::ModSourceLoversLab>(&m.source);
  REQUIRE(src != nullptr);
  REQUIRE(src->provider == "loverslab");
  REQUIRE(src->resolution == "browser");
  REQUIRE(src->section_slug == "animation");
  REQUIRE(std::get<int64_t>(src->mod_id) == 42);
  REQUIRE(src->update_policy == "exact");
}

TEST_CASE("parse_mod_entry: LoversLab with string modId", "[gmmpack][mod_entry]") {
  auto j      = mod_json();
  j["source"] = {{"provider", "loverslab"},
                 {"resolution", "browser"},
                 {"modId", "custom-string-id"},
                 {"sectionSlug", "scripts"},
                 {"updatePolicy", "latest"}};
  auto m      = gmmpack::parse_mod_entry(j);

  auto *src = std::get_if<gmmpack::ModSourceLoversLab>(&m.source);
  REQUIRE(src != nullptr);
  REQUIRE(std::get<std::string>(src->mod_id) == "custom-string-id");
  REQUIRE(src->update_policy == "latest");
}

// ---------------------------------------------------------------------------
// ModPub provider
// ---------------------------------------------------------------------------

TEST_CASE("parse_mod_entry: ModPub with int modId", "[gmmpack][mod_entry]") {
  auto j      = mod_json();
  j["source"] = {{"provider", "modpub"},
                 {"resolution", "browser"},
                 {"modId", 777},
                 {"updatePolicy", "exact"},
                 {"version", "3.1"},
                 {"sha256", "abcdef0123456789abcdef0123456789abcdef0123456789abcdef01"
                            "23456789"}};
  auto m      = gmmpack::parse_mod_entry(j);

  auto *src = std::get_if<gmmpack::ModSourceModPub>(&m.source);
  REQUIRE(src != nullptr);
  REQUIRE(src->provider == "modpub");
  REQUIRE(std::get<int64_t>(src->mod_id) == 777);
  REQUIRE(src->update_policy == "exact");
}

TEST_CASE("parse_mod_entry: ModPub with string modId", "[gmmpack][mod_entry]") {
  auto j      = mod_json();
  j["source"] = {{"provider", "modpub"},
                 {"resolution", "api"},
                 {"modId", "unique-slug"},
                 {"updatePolicy", "latest"}};
  auto m      = gmmpack::parse_mod_entry(j);

  auto *src = std::get_if<gmmpack::ModSourceModPub>(&m.source);
  REQUIRE(src != nullptr);
  REQUIRE(std::get<std::string>(src->mod_id) == "unique-slug");
}

// ---------------------------------------------------------------------------
// Steam Workshop provider
// ---------------------------------------------------------------------------

TEST_CASE("parse_mod_entry: Steam Workshop (always latest)", "[gmmpack][mod_entry]") {
  auto j      = mod_json();
  j["source"] = {{"provider", "steam_workshop"},
                 {"resolution", "client-subscription"},
                 {"appId", 72850},
                 {"workshopItemId", 12345678},
                 {"updatePolicy", "latest"}};
  auto m      = gmmpack::parse_mod_entry(j);

  auto *src = std::get_if<gmmpack::ModSourceSteamWorkshop>(&m.source);
  REQUIRE(src != nullptr);
  REQUIRE(src->provider == "steam_workshop");
  REQUIRE(src->resolution == "client-subscription");
  REQUIRE(src->app_id == 72850);
  REQUIRE(src->workshop_item_id == 12345678);
  REQUIRE(src->update_policy == "latest");
  REQUIRE_FALSE(src->version.has_value());
}

TEST_CASE("parse_mod_entry: Steam Workshop with null version", "[gmmpack][mod_entry]") {
  auto j      = mod_json();
  j["source"] = {{"provider", "steam_workshop"},
                 {"resolution", "client-subscription"},
                 {"appId", 72850},
                 {"workshopItemId", 9999},
                 {"version", nullptr},
                 {"updatePolicy", "latest"}};
  auto m      = gmmpack::parse_mod_entry(j);

  auto *src = std::get_if<gmmpack::ModSourceSteamWorkshop>(&m.source);
  REQUIRE(src != nullptr);
  REQUIRE_FALSE(src->version.has_value());
}

// ---------------------------------------------------------------------------
// Direct provider
// ---------------------------------------------------------------------------

TEST_CASE("parse_mod_entry: Direct with URL", "[gmmpack][mod_entry]") {
  auto j      = mod_json();
  j["source"] = {{"provider", "direct"},
                 {"resolution", "api"},
                 {"url", "https://example.com/mods/skyui-1.4.2.7z"},
                 {"version", "1.4.2"},
                 {"fileName", "skyui-1.4.2.7z"},
                 {"sha256", "abcdef0123456789abcdef0123456789abcdef0123456789abcdef01"
                            "23456789"},
                 {"updatePolicy", "exact"}};
  auto m      = gmmpack::parse_mod_entry(j);

  auto *src = std::get_if<gmmpack::ModSourceDirect>(&m.source);
  REQUIRE(src != nullptr);
  REQUIRE(src->provider == "direct");
  REQUIRE(src->url == "https://example.com/mods/skyui-1.4.2.7z");
  REQUIRE(src->update_policy == "exact");
}

TEST_CASE("parse_mod_entry: Direct latest (no version/hash)", "[gmmpack][mod_entry]") {
  auto j      = mod_json();
  j["source"] = {{"provider", "direct"},
                 {"resolution", "browser"},
                 {"url", "https://example.com/latest.zip"},
                 {"updatePolicy", "latest"}};
  auto m      = gmmpack::parse_mod_entry(j);

  auto *src = std::get_if<gmmpack::ModSourceDirect>(&m.source);
  REQUIRE(src != nullptr);
  REQUIRE(src->update_policy == "latest");
  REQUIRE_FALSE(src->version.has_value());
  REQUIRE_FALSE(src->sha256.has_value());
}

// ---------------------------------------------------------------------------
// Category enum
// ---------------------------------------------------------------------------

TEST_CASE("parse_mod_entry: category required", "[gmmpack][mod_entry]") {
  auto j        = mod_json();
  j["category"] = "required";
  auto m        = gmmpack::parse_mod_entry(j);
  REQUIRE(m.category == gmmpack::ModCategory::Required);
}

TEST_CASE("parse_mod_entry: category optional", "[gmmpack][mod_entry]") {
  auto j        = mod_json();
  j["category"] = "optional";
  auto m        = gmmpack::parse_mod_entry(j);
  REQUIRE(m.category == gmmpack::ModCategory::Optional);
}

TEST_CASE("parse_mod_entry: category recommended", "[gmmpack][mod_entry]") {
  auto j        = mod_json();
  j["category"] = "recommended";
  auto m        = gmmpack::parse_mod_entry(j);
  REQUIRE(m.category == gmmpack::ModCategory::Recommended);
}

TEST_CASE("parse_mod_entry: category default on empty string", "[gmmpack][mod_entry]") {
  auto j        = mod_json();
  j["category"] = "";
  auto m        = gmmpack::parse_mod_entry(j);
  REQUIRE(m.category == gmmpack::ModCategory::Optional);
}

// ---------------------------------------------------------------------------
// Phase
// ---------------------------------------------------------------------------

TEST_CASE("parse_mod_entry: phase default is 0", "[gmmpack][mod_entry]") {
  auto j = mod_json();
  j.erase("phase");
  auto m = gmmpack::parse_mod_entry(j);
  REQUIRE(m.phase == 0);
}

TEST_CASE("parse_mod_entry: phase parsed correctly", "[gmmpack][mod_entry]") {
  auto j     = mod_json();
  j["phase"] = 5;
  auto m     = gmmpack::parse_mod_entry(j);
  REQUIRE(m.phase == 5);
}

// ---------------------------------------------------------------------------
// InstallerChoices
// ---------------------------------------------------------------------------

TEST_CASE("parse_mod_entry: installerChoices present", "[gmmpack][mod_entry]") {
  auto j                = mod_json();
  j["installerChoices"] = {
      {"type", "fomod"},
      {"selections", {{"step1", {"optionA", "optionB"}}, {"step2", {"optionC"}}}}};
  auto m = gmmpack::parse_mod_entry(j);

  REQUIRE(m.installer_choices.has_value());
  auto &ic = *m.installer_choices;
  REQUIRE(ic.type == "fomod");
  REQUIRE(ic.selections.size() == 2);
  REQUIRE(ic.selections.count("step1"));
  REQUIRE(ic.selections.at("step1").size() == 2);
  REQUIRE(ic.selections.at("step1")[0] == "optionA");
  REQUIRE(ic.selections.at("step1")[1] == "optionB");
  REQUIRE(ic.selections.at("step2").size() == 1);
  REQUIRE(ic.selections.at("step2")[0] == "optionC");
}

TEST_CASE("parse_mod_entry: installerChoices absent", "[gmmpack][mod_entry]") {
  auto j = mod_json();
  j.erase("installerChoices");
  auto m = gmmpack::parse_mod_entry(j);
  REQUIRE_FALSE(m.installer_choices.has_value());
}

TEST_CASE("parse_mod_entry: installerChoices empty selections",
          "[gmmpack][mod_entry]") {
  auto j                = mod_json();
  j["installerChoices"] = {{"type", "fomod"}, {"selections", {}}};
  auto m                = gmmpack::parse_mod_entry(j);

  REQUIRE(m.installer_choices.has_value());
  REQUIRE(m.installer_choices->type == "fomod");
  REQUIRE(m.installer_choices->selections.empty());
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST_CASE("parse_mod_entry: unknown provider falls back to Nexus",
          "[gmmpack][mod_entry]") {
  auto j      = mod_json();
  j["source"] = {{"provider", "unknown_provider"}, {"resolution", "browser"}};
  auto m      = gmmpack::parse_mod_entry(j);

  // Unknown provider defaults to ModSourceNexus with empty fields
  auto *src = std::get_if<gmmpack::ModSourceNexus>(&m.source);
  REQUIRE(src != nullptr);
  REQUIRE(src->provider == "nexus");
}

TEST_CASE("parse_mod_entry: minimal required fields only", "[gmmpack][mod_entry]") {
  nlohmann::json j;
  j["id"]       = "minimal";
  j["name"]     = "Minimal Mod";
  j["category"] = "optional";
  j["source"]   = {{"provider", "nexus"},
                   {"resolution", "api"},
                   {"gameDomain", "skyrim"},
                   {"modId", 1}};
  auto m        = gmmpack::parse_mod_entry(j);

  REQUIRE(m.id == "minimal");
  REQUIRE(m.phase == 0);
  REQUIRE(m.category == gmmpack::ModCategory::Optional);

  auto *src = std::get_if<gmmpack::ModSourceNexus>(&m.source);
  REQUIRE(src != nullptr);
  REQUIRE(src->mod_id == 1);
  REQUIRE(src->update_policy == "exact");
  REQUIRE_FALSE(src->file_id.has_value());
}
