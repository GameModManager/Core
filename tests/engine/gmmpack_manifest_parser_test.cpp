// Tests for the gmmpack manifest parser.
//
// Covers:
//   - parse_semver() validation
//   - parse_manifest() JSON -> Manifest struct conversion
//   - Schema-level validation of manifest.json (unknown fields, bad UUID,
//     missing required fields, unknown schema versions)
//
// These tests exercise the parser in isolation from the archive pipeline.

#include "engine/gmmpack/schema_validator.h"
#include "engine/gmmpack/types.h"
#include "engine/gmmpack/unpacker.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace gmmpack = engine::gmmpack;

// ---------------------------------------------------------------------------
// Helper: load the manifest schema from the input/ directory
// ---------------------------------------------------------------------------

static nlohmann::json load_manifest_schema() {
  auto project_root = std::filesystem::path(PROJECT_SOURCE_DIR);
  auto candidate    = project_root / ".." / ".." / "input" / "manifest.schema.json";
  if (std::filesystem::is_regular_file(candidate))
    candidate = std::filesystem::canonical(candidate);
  else {
    candidate = project_root / ".." / "input" / "manifest.schema.json";
    if (std::filesystem::is_regular_file(candidate))
      candidate = std::filesystem::canonical(candidate);
    else
      FAIL("manifest.schema.json not found");
  }
  std::ifstream f(candidate);
  return nlohmann::json::parse(f);
}

// ---------------------------------------------------------------------------
// Minimal valid manifest JSON
// ---------------------------------------------------------------------------

static nlohmann::json minimal_manifest() {
  nlohmann::json j;
  j["gmmpackSchema"] = "1.0.0";
  j["id"]            = "b3f1e2a0-1234-4abc-8def-000000000001";
  j["revision"]      = 1;
  j["info"]          = {{"name", "Test Pack"},
                        {"author", "Test Author"},
                        {"gmmGameId", "skyrim_se"},
                        {"createdAt", "2026-09-01T00:00:00Z"},
                        {"updatedAt", "2026-09-14T00:00:00Z"}};
  j["archive"] = {{"fileHashes",
                   {{"tree.json", "sha256:00000000000000000000000000000000000000000000"
                                  "00000000000000000000"}}}};
  return j;
}

// ===========================================================================
// SECTION 1: parse_semver()
// ===========================================================================

TEST_CASE("parse_semver accepts valid semver", "[gmmpack][semver]") {
  auto r = gmmpack::parse_semver("1.0.0");
  REQUIRE(r.has_value());
  REQUIRE(r->major == 1);
  REQUIRE(r->minor == 0);
  REQUIRE(r->patch == 0);
}

TEST_CASE("parse_semver accepts 1.2.3", "[gmmpack][semver]") {
  auto r = gmmpack::parse_semver("1.2.3");
  REQUIRE(r.has_value());
  REQUIRE(r->major == 1);
  REQUIRE(r->minor == 2);
  REQUIRE(r->patch == 3);
}

TEST_CASE("parse_semver accepts large numbers", "[gmmpack][semver]") {
  auto r = gmmpack::parse_semver("12.345.6789");
  REQUIRE(r.has_value());
  REQUIRE(r->major == 12);
  REQUIRE(r->minor == 345);
  REQUIRE(r->patch == 6789);
}

TEST_CASE("parse_semver rejects empty string", "[gmmpack][semver]") {
  REQUIRE_FALSE(gmmpack::parse_semver("").has_value());
}

TEST_CASE("parse_semver rejects no patch component", "[gmmpack][semver]") {
  REQUIRE_FALSE(gmmpack::parse_semver("1.0").has_value());
}

TEST_CASE("parse_semver rejects no minor component", "[gmmpack][semver]") {
  REQUIRE_FALSE(gmmpack::parse_semver("1").has_value());
}

TEST_CASE("parse_semver rejects leading dot", "[gmmpack][semver]") {
  REQUIRE_FALSE(gmmpack::parse_semver(".0.0").has_value());
}

TEST_CASE("parse_semver rejects trailing dot", "[gmmpack][semver]") {
  REQUIRE_FALSE(gmmpack::parse_semver("1.0.0.").has_value());
}

TEST_CASE("parse_semver rejects trailing text", "[gmmpack][semver]") {
  REQUIRE_FALSE(gmmpack::parse_semver("1.0.0-beta").has_value());
}

TEST_CASE("parse_semver rejects non-numeric", "[gmmpack][semver]") {
  REQUIRE_FALSE(gmmpack::parse_semver("1.0.x").has_value());
}

TEST_CASE("parse_semver rejects spaces", "[gmmpack][semver]") {
  REQUIRE_FALSE(gmmpack::parse_semver("1 .0.0").has_value());
}

TEST_CASE("parse_semver rejects negative numbers", "[gmmpack][semver]") {
  REQUIRE_FALSE(gmmpack::parse_semver("-1.0.0").has_value());
}

// ===========================================================================
// SECTION 2: parse_manifest() JSON -> Manifest struct
// ===========================================================================

TEST_CASE("parse_manifest reads all required fields", "[gmmpack][manifest]") {
  auto j = minimal_manifest();
  auto m = gmmpack::parse_manifest(j);

  REQUIRE(m.gmmpack_schema == "1.0.0");
  REQUIRE(m.id == "b3f1e2a0-1234-4abc-8def-000000000001");
  REQUIRE(m.revision == 1);
  REQUIRE(m.info.name == "Test Pack");
  REQUIRE(m.info.author == "Test Author");
  REQUIRE(m.info.gmm_game_id == "skyrim_se");
  REQUIRE(m.info.created_at == "2026-09-01T00:00:00Z");
  REQUIRE(m.info.updated_at == "2026-09-14T00:00:00Z");
  REQUIRE_FALSE(m.archive.file_hashes.empty());
}

TEST_CASE("parse_manifest reads info.description when present", "[gmmpack][manifest]") {
  auto j                   = minimal_manifest();
  j["info"]["description"] = "A test pack";
  auto m                   = gmmpack::parse_manifest(j);
  REQUIRE(m.info.description == "A test pack");
}

TEST_CASE("parse_manifest reads info.homepage when present", "[gmmpack][manifest]") {
  auto j                = minimal_manifest();
  j["info"]["homepage"] = "https://example.com";
  auto m                = gmmpack::parse_manifest(j);
  REQUIRE(m.info.homepage == "https://example.com");
}

TEST_CASE("parse_manifest reads tools array", "[gmmpack][manifest]") {
  auto j     = minimal_manifest();
  j["tools"] = {
      {{"id", "loot"}, {"name", "LOOT"}, {"homepage", "https://loot.github.io"}}};
  auto m = gmmpack::parse_manifest(j);

  REQUIRE(m.tools.size() == 1);
  REQUIRE(m.tools[0].id == "loot");
  REQUIRE(m.tools[0].name == "LOOT");
  REQUIRE(m.tools[0].homepage == "https://loot.github.io");
}

TEST_CASE("parse_manifest reads multiple tools", "[gmmpack][manifest]") {
  auto j     = minimal_manifest();
  j["tools"] = {{{"id", "loot"}, {"name", "LOOT"}},
                {{"id", "xedit"}, {"name", "xEdit"}}};
  auto m     = gmmpack::parse_manifest(j);
  REQUIRE(m.tools.size() == 2);
  REQUIRE(m.tools[0].id == "loot");
  REQUIRE(m.tools[1].id == "xedit");
}

TEST_CASE("parse_manifest reads platform block", "[gmmpack][manifest]") {
  auto j        = minimal_manifest();
  j["platform"] = {{"linux",
                    {{"protonVersionPin", "GE-Proton9-27"},
                     {"steamOverlay", true},
                     {"launchOptions", "OPT=1"}}},
                   {"windows", {{"launchOptions", "none"}}}};
  auto m        = gmmpack::parse_manifest(j);

  REQUIRE(m.platform.linux_plat.has_value());
  REQUIRE(m.platform.linux_plat->proton_version_pin == "GE-Proton9-27");
  REQUIRE(m.platform.linux_plat->steam_overlay == true);
  REQUIRE(m.platform.linux_plat->launch_options == "OPT=1");
  REQUIRE_FALSE(m.platform.macos.has_value());
  REQUIRE(m.platform.windows.has_value());
  REQUIRE(m.platform.windows->launch_options == "none");
}

TEST_CASE("parse_manifest reads platform prefixFiles", "[gmmpack][manifest]") {
  auto j        = minimal_manifest();
  j["platform"] = {
      {"linux",
       {{"prefixFiles", {{{"path", "drive_c/foo"}, {"sourceModId", "foo-mod"}}}}}}};
  auto m = gmmpack::parse_manifest(j);

  REQUIRE(m.platform.linux_plat.has_value());
  REQUIRE(m.platform.linux_plat->prefix_files.size() == 1);
  REQUIRE(m.platform.linux_plat->prefix_files[0].path == "drive_c/foo");
  REQUIRE(m.platform.linux_plat->prefix_files[0].source_mod_id == "foo-mod");
}

TEST_CASE("parse_manifest reads rules", "[gmmpack][manifest]") {
  auto j     = minimal_manifest();
  j["rules"] = {
      {{"type", "requires"}, {"from", "mod-a"}, {"to", "mod-b"}},
      {{"type", "before"}, {"from", "mod-c"}, {"to", "mod-d"}, {"note", "reason"}}};
  auto m = gmmpack::parse_manifest(j);

  REQUIRE(m.rules.size() == 2);
  REQUIRE(m.rules[0].type == "requires");
  REQUIRE(m.rules[0].from == "mod-a");
  REQUIRE(m.rules[0].to == "mod-b");
  REQUIRE(m.rules[0].note.empty());
  REQUIRE(m.rules[1].type == "before");
  REQUIRE(m.rules[1].note == "reason");
}

TEST_CASE("parse_manifest reads loadOrder.pluginHint", "[gmmpack][manifest]") {
  auto j         = minimal_manifest();
  j["loadOrder"] = {{"pluginHint", {"A.esp", "B.esp", "C.esp"}}};
  auto m         = gmmpack::parse_manifest(j);

  REQUIRE(m.load_order.plugin_hint.size() == 3);
  REQUIRE(m.load_order.plugin_hint[0] == "A.esp");
  REQUIRE(m.load_order.plugin_hint[2] == "C.esp");
}

TEST_CASE("parse_manifest reads choiceGroups", "[gmmpack][manifest]") {
  auto j            = minimal_manifest();
  j["choiceGroups"] = {{{"id", "body-type"},
                        {"name", "Body Type"},
                        {"mode", "exactly-one"},
                        {"memberModIds", {"cbbe", "unp"}}},
                       {{"id", "enb"},
                        {"name", "ENB Choice"},
                        {"mode", "at-most-one"},
                        {"memberModIds", {"rudy", "natural", "cyber"}}}};
  auto m            = gmmpack::parse_manifest(j);

  REQUIRE(m.choice_groups.size() == 2);
  REQUIRE(m.choice_groups[0].id == "body-type");
  REQUIRE(m.choice_groups[0].mode == "exactly-one");
  REQUIRE(m.choice_groups[0].member_mod_ids.size() == 2);
  REQUIRE(m.choice_groups[1].id == "enb");
  REQUIRE(m.choice_groups[1].mode == "at-most-one");
  REQUIRE(m.choice_groups[1].member_mod_ids.size() == 3);
}

TEST_CASE("parse_manifest reads archive.fileHashes", "[gmmpack][manifest]") {
  auto j = minimal_manifest();
  j["archive"]["fileHashes"]["mods/skyui.json"] =
      "sha256:abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  j["archive"]["fileHashes"]["tree.json"] =
      "sha256:1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef";
  auto m = gmmpack::parse_manifest(j);

  REQUIRE(m.archive.file_hashes.size() == 2);
  REQUIRE(m.archive.file_hashes.count("mods/skyui.json"));
  REQUIRE(m.archive.file_hashes.count("tree.json"));
}

TEST_CASE("parse_manifest defaults for missing optional fields",
          "[gmmpack][manifest]") {
  auto j = minimal_manifest();
  auto m = gmmpack::parse_manifest(j);

  REQUIRE(m.info.description.empty());
  REQUIRE(m.info.homepage.empty());
  REQUIRE(m.tools.empty());
  REQUIRE_FALSE(m.platform.linux_plat.has_value());
  REQUIRE_FALSE(m.platform.macos.has_value());
  REQUIRE_FALSE(m.platform.windows.has_value());
  REQUIRE(m.rules.empty());
  REQUIRE(m.load_order.plugin_hint.empty());
  REQUIRE(m.choice_groups.empty());
}

TEST_CASE("parse_manifest with empty JSON produces defaults", "[gmmpack][manifest]") {
  auto m = gmmpack::parse_manifest(nlohmann::json::object());

  REQUIRE(m.gmmpack_schema.empty());
  REQUIRE(m.id.empty());
  REQUIRE(m.revision == 0);
  REQUIRE(m.info.name.empty());
  REQUIRE(m.tools.empty());
  REQUIRE(m.rules.empty());
}

TEST_CASE("parse_manifest with high revision", "[gmmpack][manifest]") {
  auto j        = minimal_manifest();
  j["revision"] = 999;
  auto m        = gmmpack::parse_manifest(j);
  REQUIRE(m.revision == 999);
}

TEST_CASE("parse_manifest preserves multiple file hashes", "[gmmpack][manifest]") {
  auto j   = minimal_manifest();
  auto &fh = j["archive"]["fileHashes"];
  for (int i = 0; i < 5; ++i) {
    auto path = "mods/mod_" + std::to_string(i) + ".json";
    fh[path]  = "sha256:" + std::string(64, 'a' + i);
  }
  auto m = gmmpack::parse_manifest(j);
  REQUIRE(m.archive.file_hashes.size() == 6);  // 5 + tree.json
}

// ===========================================================================
// SECTION 3: Schema-level manifest validation (unknown fields, bad UUID,
//            missing required fields, unknown schema versions)
// ===========================================================================

TEST_CASE("schema rejects unknown top-level property in manifest",
          "[gmmpack][schema]") {
  auto schema          = load_manifest_schema();
  auto j               = minimal_manifest();
  j["unexpectedField"] = 42;

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
  bool found_unknown = false;
  for (const auto &d : diag) {
    if (d.message.find("unknown property") != std::string::npos) {
      found_unknown = true;
      break;
    }
  }
  REQUIRE(found_unknown);
}

TEST_CASE("schema rejects unknown property inside info", "[gmmpack][schema]") {
  auto schema             = load_manifest_schema();
  auto j                  = minimal_manifest();
  j["info"]["bogusField"] = "nope";

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
  bool found_unknown = false;
  for (const auto &d : diag) {
    if (d.message.find("unknown property") != std::string::npos) {
      found_unknown = true;
      break;
    }
  }
  REQUIRE(found_unknown);
}

TEST_CASE("schema rejects unknown property inside tools items", "[gmmpack][schema]") {
  auto schema = load_manifest_schema();
  auto j      = minimal_manifest();
  j["tools"]  = {{{"id", "loot"}, {"name", "LOOT"}, {"extra", "nope"}}};

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
}

TEST_CASE("schema rejects missing required field: id", "[gmmpack][schema]") {
  auto schema = load_manifest_schema();
  auto j      = minimal_manifest();
  j.erase("id");

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
  bool found_required = false;
  for (const auto &d : diag) {
    if (d.message.find("missing required") != std::string::npos) {
      found_required = true;
      break;
    }
  }
  REQUIRE(found_required);
}

TEST_CASE("schema rejects missing required field: revision", "[gmmpack][schema]") {
  auto schema = load_manifest_schema();
  auto j      = minimal_manifest();
  j.erase("revision");

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
}

TEST_CASE("schema rejects missing required field: info", "[gmmpack][schema]") {
  auto schema = load_manifest_schema();
  auto j      = minimal_manifest();
  j.erase("info");

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
}

TEST_CASE("schema rejects missing required field: archive", "[gmmpack][schema]") {
  auto schema = load_manifest_schema();
  auto j      = minimal_manifest();
  j.erase("archive");

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
}

TEST_CASE("schema rejects invalid UUID format", "[gmmpack][schema]") {
  auto schema = load_manifest_schema();
  auto j      = minimal_manifest();
  j["id"]     = "not-a-uuid";

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
  bool found_uuid = false;
  for (const auto &d : diag) {
    if (d.message.find("UUID") != std::string::npos) {
      found_uuid = true;
      break;
    }
  }
  REQUIRE(found_uuid);
}

TEST_CASE("schema rejects gmmpackSchema with bad pattern", "[gmmpack][schema]") {
  auto schema        = load_manifest_schema();
  auto j             = minimal_manifest();
  j["gmmpackSchema"] = "not-semver";

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
  bool found_pattern = false;
  for (const auto &d : diag) {
    if (d.message.find("pattern") != std::string::npos ||
        d.message.find("does not match") != std::string::npos) {
      found_pattern = true;
      break;
    }
  }
  REQUIRE(found_pattern);
}

TEST_CASE("schema rejects revision below minimum", "[gmmpack][schema]") {
  auto schema   = load_manifest_schema();
  auto j        = minimal_manifest();
  j["revision"] = 0;

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
  bool found_minimum = false;
  for (const auto &d : diag) {
    if (d.message.find("minimum") != std::string::npos ||
        d.message.find("< minimum") != std::string::npos) {
      found_minimum = true;
      break;
    }
  }
  REQUIRE(found_minimum);
}

TEST_CASE("schema rejects missing info subfield: name", "[gmmpack][schema]") {
  auto schema = load_manifest_schema();
  auto j      = minimal_manifest();
  j["info"].erase("name");

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
}

TEST_CASE("schema rejects missing info subfield: author", "[gmmpack][schema]") {
  auto schema = load_manifest_schema();
  auto j      = minimal_manifest();
  j["info"].erase("author");

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
}

TEST_CASE("schema rejects missing info subfield: gmmGameId", "[gmmpack][schema]") {
  auto schema = load_manifest_schema();
  auto j      = minimal_manifest();
  j["info"].erase("gmmGameId");

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
}

TEST_CASE("schema rejects bad rule type enum", "[gmmpack][schema]") {
  auto schema = load_manifest_schema();
  auto j      = minimal_manifest();
  j["rules"]  = {{{"type", "INVALID"}, {"from", "a"}, {"to", "b"}}};

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
  bool found_enum = false;
  for (const auto &d : diag) {
    if (d.message.find("not in enum") != std::string::npos) {
      found_enum = true;
      break;
    }
  }
  REQUIRE(found_enum);
}

TEST_CASE("schema rejects choiceGroup mode enum", "[gmmpack][schema]") {
  auto schema       = load_manifest_schema();
  auto j            = minimal_manifest();
  j["choiceGroups"] = {
      {{"id", "g"}, {"name", "G"}, {"mode", "INVALID"}, {"memberModIds", {"a", "b"}}}};

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
  bool found_enum = false;
  for (const auto &d : diag) {
    if (d.message.find("not in enum") != std::string::npos) {
      found_enum = true;
      break;
    }
  }
  REQUIRE(found_enum);
}

TEST_CASE("schema rejects choiceGroup with fewer than 2 members", "[gmmpack][schema]") {
  auto schema       = load_manifest_schema();
  auto j            = minimal_manifest();
  j["choiceGroups"] = {
      {{"id", "g"}, {"name", "G"}, {"mode", "exactly-one"}, {"memberModIds", {"a"}}}};

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
  bool found_min = false;
  for (const auto &d : diag) {
    if (d.message.find("too short") != std::string::npos ||
        d.message.find("minItems") != std::string::npos ||
        d.message.find("min") != std::string::npos) {
      found_min = true;
      break;
    }
  }
  REQUIRE(found_min);
}

TEST_CASE("schema rejects empty archive.fileHashes", "[gmmpack][schema]") {
  auto schema                = load_manifest_schema();
  auto j                     = minimal_manifest();
  j["archive"]["fileHashes"] = nlohmann::json::object();

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
}

TEST_CASE("schema accepts valid manifest with all optional fields",
          "[gmmpack][schema]") {
  auto schema              = load_manifest_schema();
  auto j                   = minimal_manifest();
  j["info"]["description"] = "Full pack";
  j["info"]["homepage"]    = "https://example.com";
  j["tools"]               = {{{"id", "loot"}, {"name", "LOOT"}}};
  j["platform"]            = {
      {"linux", {{"protonVersionPin", "GE-Proton9-27"}, {"steamOverlay", true}}}};
  j["rules"]        = {{{"type", "requires"}, {"from", "a"}, {"to", "b"}}};
  j["loadOrder"]    = {{"pluginHint", {"A.esp"}}};
  j["choiceGroups"] = {{{"id", "g"},
                        {"name", "G"},
                        {"mode", "at-most-one"},
                        {"memberModIds", {"a", "b"}}}};

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE(diag.empty());
}

// ===========================================================================
// SECTION 4: Semver major-version policy (integration with unpacker)
// ===========================================================================

TEST_CASE("schema rejects gmmpackSchema with minor-only version", "[gmmpack][schema]") {
  auto schema        = load_manifest_schema();
  auto j             = minimal_manifest();
  j["gmmpackSchema"] = "1";

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
}

TEST_CASE("schema rejects gmmpackSchema with pre-release suffix", "[gmmpack][schema]") {
  auto schema        = load_manifest_schema();
  auto j             = minimal_manifest();
  j["gmmpackSchema"] = "1.0.0-beta.1";

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
}

TEST_CASE("parse_semver accepts 1.0.0 exactly", "[gmmpack][semver]") {
  auto r = gmmpack::parse_semver("1.0.0");
  REQUIRE(r.has_value());
  REQUIRE(r->major == 1);
}

TEST_CASE("schema validates info.createdAt format", "[gmmpack][schema]") {
  auto schema            = load_manifest_schema();
  auto j                 = minimal_manifest();
  j["info"]["createdAt"] = "not-a-date";

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
  bool found_format = false;
  for (const auto &d : diag) {
    if (d.message.find("date-time") != std::string::npos ||
        d.message.find("format") != std::string::npos) {
      found_format = true;
      break;
    }
  }
  REQUIRE(found_format);
}

TEST_CASE("schema validates info.homepage as URI", "[gmmpack][schema]") {
  auto schema           = load_manifest_schema();
  auto j                = minimal_manifest();
  j["info"]["homepage"] = "not-a-uri";

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
  bool found_format = false;
  for (const auto &d : diag) {
    if (d.message.find("URI") != std::string::npos ||
        d.message.find("format") != std::string::npos) {
      found_format = true;
      break;
    }
  }
  REQUIRE(found_format);
}

TEST_CASE("schema rejects empty info.name", "[gmmpack][schema]") {
  auto schema       = load_manifest_schema();
  auto j            = minimal_manifest();
  j["info"]["name"] = "";

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
  bool found_min = false;
  for (const auto &d : diag) {
    if (d.message.find("too short") != std::string::npos) {
      found_min = true;
      break;
    }
  }
  REQUIRE(found_min);
}

TEST_CASE("schema rejects empty info.author", "[gmmpack][schema]") {
  auto schema         = load_manifest_schema();
  auto j              = minimal_manifest();
  j["info"]["author"] = "";

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
  bool found_min = false;
  for (const auto &d : diag) {
    if (d.message.find("too short") != std::string::npos) {
      found_min = true;
      break;
    }
  }
  REQUIRE(found_min);
}

TEST_CASE("schema rejects platform with unknown property", "[gmmpack][schema]") {
  auto schema   = load_manifest_schema();
  auto j        = minimal_manifest();
  j["platform"] = {{"linux", {{"unknownProp", true}}}};

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
  bool found_unknown = false;
  for (const auto &d : diag) {
    if (d.message.find("unknown property") != std::string::npos) {
      found_unknown = true;
      break;
    }
  }
  REQUIRE(found_unknown);
}

TEST_CASE("schema rejects platform with extra key", "[gmmpack][schema]") {
  auto schema   = load_manifest_schema();
  auto j        = minimal_manifest();
  j["platform"] = {{"beos", nlohmann::json::object()}};

  gmmpack::SchemaValidator v;
  auto diag = v.validate(j, schema, schema);
  REQUIRE_FALSE(diag.empty());
}
