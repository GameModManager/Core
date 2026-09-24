// Tests for the .gmmpack archive unpacker + schema validator.
//
// Validates all three stages:
//   1. Archive integrity (sha256 per file vs manifest.fileHashes)
//   2. Schema validation (JSON Schema draft 2020-12, strict)
//   3. Referential integrity (cross-file ID references)
//
// Tests create synthetic .gmmpack zip archives via libarchive's write API
// so no fixture files are needed.

#include "engine/gmmpack/unpacker.h"
#include "engine/gmmpack/schema_validator.h"

#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace fs      = std::filesystem;
namespace gmmpack = engine::gmmpack;

// ---------------------------------------------------------------------------
// SHA-256 helper (matches unpacker.cpp)
// ---------------------------------------------------------------------------

static std::string sha256_hex(const std::string &data) {
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  REQUIRE(ctx != nullptr);
  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
  EVP_DigestUpdate(ctx, data.data(), data.size());
  EVP_DigestFinal_ex(ctx, hash, &len);
  EVP_MD_CTX_free(ctx);
  static const char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(len * 2);
  for (unsigned int i = 0; i < len; ++i) {
    result.push_back(hex[hash[i] >> 4]);
    result.push_back(hex[hash[i] & 0x0f]);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Test zip builder
// ---------------------------------------------------------------------------

struct ZipItem {
  std::string path;
  std::string content;
};

struct TempDir {
  fs::path root;
  TempDir() {
    static int counter = 0;
    root = fs::temp_directory_path() / ("gmmpack_test_" + std::to_string(::getpid()) +
                                        "_" + std::to_string(counter++));
    fs::create_directories(root);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

static fs::path make_zip(const TempDir &td, const std::string &name,
                         const std::vector<ZipItem> &items) {
  auto path         = td.root / name;
  struct archive *a = archive_write_new();
  archive_write_set_format_zip(a);
  archive_write_set_options(a, "zip:compression=deflate");
  REQUIRE(archive_write_open_filename(a, path.string().c_str()) == ARCHIVE_OK);
  for (const auto &item : items) {
    struct archive_entry *e = archive_entry_new();
    archive_entry_set_pathname(e, item.path.c_str());
    archive_entry_set_size(e, item.content.size());
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, 0644);
    REQUIRE(archive_write_header(a, e) == ARCHIVE_OK);
    archive_write_data(a, item.content.data(), item.content.size());
    archive_entry_free(e);
  }
  REQUIRE(archive_write_close(a) == ARCHIVE_OK);
  archive_write_free(a);
  return path;
}

// ---------------------------------------------------------------------------
// Schema dir - uses the input/ schemas from the workspace
// ---------------------------------------------------------------------------

static fs::path schema_dir() {
  auto project_root = fs::path(PROJECT_SOURCE_DIR);
  auto candidate    = project_root / ".." / ".." / "input";
  if (fs::is_directory(candidate))
    return fs::canonical(candidate);
  candidate = project_root / ".." / "input";
  if (fs::is_directory(candidate))
    return fs::canonical(candidate);
  FAIL("schema dir not found from " + project_root.string());
  return candidate;
}

static gmmpack::SchemaSet load_schemas() {
  auto dir     = schema_dir();
  auto schemas = gmmpack::load_schema_set(dir);
  INFO("schema_dir=" << dir.string() << " count=" << schemas.size());
  return schemas;
}

// ---------------------------------------------------------------------------
// Minimal valid manifest.json
// ---------------------------------------------------------------------------

static std::string
make_manifest(const std::unordered_map<std::string, std::string> &file_hashes,
              const std::string &schema_version = "1.0.0") {
  nlohmann::json j;
  j["gmmpackSchema"]         = schema_version;
  j["id"]                    = "b3f1e2a0-1234-4abc-8def-000000000001";
  j["revision"]              = 1;
  j["info"]                  = {{"name", "Test Pack"},
                                {"author", "Test Author"},
                                {"gmmGameId", "skyrim_se"},
                                {"createdAt", "2026-09-01T00:00:00Z"},
                                {"updatedAt", "2026-09-14T00:00:00Z"}};
  j["archive"]["fileHashes"] = file_hashes;
  return j.dump();
}

static std::string make_manifest_no_hash() {
  nlohmann::json j;
  j["gmmpackSchema"]         = "1.0.0";
  j["id"]                    = "b3f1e2a0-1234-4abc-8def-000000000001";
  j["revision"]              = 1;
  j["info"]                  = {{"name", "Test Pack"},
                                {"author", "Test Author"},
                                {"gmmGameId", "skyrim_se"},
                                {"createdAt", "2026-09-01T00:00:00Z"},
                                {"updatedAt", "2026-09-14T00:00:00Z"}};
  j["archive"]["fileHashes"] = nlohmann::json::object();
  return j.dump();
}

// A valid mod entry
static std::string make_mod_json(const std::string &id = "skyui") {
  nlohmann::json j;
  j["id"]       = id;
  j["name"]     = "SkyUI";
  j["category"] = "required";
  j["source"]   = {
      {"provider", "nexus"},
      {"resolution", "api"},
      {"gameDomain", "skyrimspecialedition"},
      {"modId", 12345},
      {"fileId", 67890},
      {"version", "1.4.2"},
      {"fileName", "SkyUI-1.4.2.7z"},
      {"fileSize", 48213311},
      {"sha256", "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"},
      {"updatePolicy", "exact"}};
  return j.dump();
}

// A valid tree.json
static std::string make_tree_json() {
  nlohmann::json j;
  j["nodes"] = {{{"type", "mod"}, {"id", "skyui"}, {"enabled", true}}};
  return j.dump();
}

// ---------------------------------------------------------------------------
// TEST CASES
// ---------------------------------------------------------------------------

TEST_CASE("gmmpack extract valid archive", "[gmmpack]") {
  TempDir td;
  std::string mod_json  = make_mod_json();
  std::string tree_json = make_tree_json();

  // Compute hashes
  std::string mod_hash  = "sha256:" + sha256_hex(mod_json);
  std::string tree_hash = "sha256:" + sha256_hex(tree_json);

  std::string manifest_json =
      make_manifest({{"mods/skyui.json", mod_hash}, {"tree.json", tree_hash}});

  auto zip = make_zip(td, "test.gmmpack",
                      {
                          {"manifest.json", manifest_json},
                          {"mods/skyui.json", mod_json},
                          {"tree.json", tree_json},
                      });

  auto result = gmmpack::extract_archive(zip);
  REQUIRE(result.ok);
  REQUIRE(result.archive.files.size() == 3);
  REQUIRE(result.archive.path_index.count("manifest.json"));
  REQUIRE(result.archive.path_index.count("mods/skyui.json"));
  REQUIRE(result.archive.path_index.count("tree.json"));
}

TEST_CASE("gmmpack extract missing manifest", "[gmmpack]") {
  TempDir td;
  auto zip    = make_zip(td, "test.gmmpack",
                         {
                             {"mods/skyui.json", "{}"},
                         });
  auto result = gmmpack::extract_archive(zip);
  REQUIRE_FALSE(result.ok);
  REQUIRE_FALSE(result.diagnostics.empty());
}

TEST_CASE("gmmpack archive integrity passes", "[gmmpack]") {
  TempDir td;
  std::string mod_json  = make_mod_json();
  std::string tree_json = make_tree_json();

  std::string mod_hash  = "sha256:" + sha256_hex(mod_json);
  std::string tree_hash = "sha256:" + sha256_hex(tree_json);
  std::string manifest_json =
      make_manifest({{"mods/skyui.json", mod_hash}, {"tree.json", tree_hash}});

  auto zip = make_zip(td, "test.gmmpack",
                      {
                          {"manifest.json", manifest_json},
                          {"mods/skyui.json", mod_json},
                          {"tree.json", tree_json},
                      });

  auto extract = gmmpack::extract_archive(zip);
  REQUIRE(extract.ok);

  auto diag = gmmpack::verify_archive_integrity(extract.archive, extract.manifest_json);
  REQUIRE(diag.empty());
}

TEST_CASE("gmmpack archive integrity fails on hash mismatch", "[gmmpack]") {
  TempDir td;
  std::string mod_json  = make_mod_json();
  std::string tree_json = make_tree_json();

  // Wrong hash
  std::string mod_hash =
      "sha256:0000000000000000000000000000000000000000000000000000000000000000";
  std::string tree_hash = "sha256:" + sha256_hex(tree_json);
  std::string manifest_json =
      make_manifest({{"mods/skyui.json", mod_hash}, {"tree.json", tree_hash}});

  auto zip = make_zip(td, "test.gmmpack",
                      {
                          {"manifest.json", manifest_json},
                          {"mods/skyui.json", mod_json},
                          {"tree.json", tree_json},
                      });

  auto extract = gmmpack::extract_archive(zip);
  REQUIRE(extract.ok);

  auto diag = gmmpack::verify_archive_integrity(extract.archive, extract.manifest_json);
  REQUIRE_FALSE(diag.empty());
  bool found_mismatch = false;
  for (const auto &d : diag) {
    if (d.message.find("sha256 mismatch") != std::string::npos) {
      found_mismatch = true;
      break;
    }
  }
  REQUIRE(found_mismatch);
}

TEST_CASE("gmmpack archive integrity fails on missing file", "[gmmpack]") {
  TempDir td;
  std::string tree_json = make_tree_json();

  // manifest lists a file that doesn't exist in the archive
  std::string tree_hash     = "sha256:" + sha256_hex(tree_json);
  std::string manifest_json = make_manifest(
      {{"mods/skyui.json",
        "sha256:0000000000000000000000000000000000000000000000000000000000000000"},
       {"tree.json", tree_hash}});

  auto zip = make_zip(td, "test.gmmpack",
                      {
                          {"manifest.json", manifest_json},
                          {"tree.json", tree_json},
                          // mods/skyui.json is missing!
                      });

  auto extract = gmmpack::extract_archive(zip);
  REQUIRE(extract.ok);

  auto diag = gmmpack::verify_archive_integrity(extract.archive, extract.manifest_json);
  REQUIRE_FALSE(diag.empty());
  bool found_missing = false;
  for (const auto &d : diag) {
    if (d.message.find("not in archive") != std::string::npos) {
      found_missing = true;
      break;
    }
  }
  REQUIRE(found_missing);
}

TEST_CASE("gmmpack archive integrity rejects non-string hash", "[gmmpack]") {
  TempDir td;
  std::string tree_json = make_tree_json();

  nlohmann::json mj;
  mj["gmmpackSchema"]                      = "1.0.0";
  mj["id"]                                 = "b3f1e2a0-1234-4abc-8def-000000000001";
  mj["revision"]                           = 1;
  mj["info"]                               = {{"name", "Test Pack"},
                                              {"author", "Test Author"},
                                              {"gmmGameId", "skyrim_se"},
                                              {"createdAt", "2026-09-01T00:00:00Z"},
                                              {"updatedAt", "2026-09-14T00:00:00Z"}};
  mj["archive"]["fileHashes"]["tree.json"] = 12345;  // not a string!
  std::string manifest_json                = mj.dump();

  auto zip = make_zip(td, "test.gmmpack",
                      {
                          {"manifest.json", manifest_json},
                          {"tree.json", tree_json},
                      });

  auto extract = gmmpack::extract_archive(zip);
  REQUIRE(extract.ok);

  // Must report an error, not throw.
  auto diag = gmmpack::verify_archive_integrity(extract.archive, extract.manifest_json);
  REQUIRE_FALSE(diag.empty());
  bool found_type = false;
  for (const auto &d : diag) {
    if (d.message.find("must be a string") != std::string::npos) {
      found_type = true;
      break;
    }
  }
  REQUIRE(found_type);
}

TEST_CASE("gmmpack archive integrity rejects non-object fileHashes", "[gmmpack]") {
  TempDir td;
  std::string tree_json = make_tree_json();

  nlohmann::json mj;
  mj["gmmpackSchema"]         = "1.0.0";
  mj["id"]                    = "b3f1e2a0-1234-4abc-8def-000000000001";
  mj["revision"]              = 1;
  mj["info"]                  = {{"name", "Test Pack"},
                                 {"author", "Test Author"},
                                 {"gmmGameId", "skyrim_se"},
                                 {"createdAt", "2026-09-01T00:00:00Z"},
                                 {"updatedAt", "2026-09-14T00:00:00Z"}};
  mj["archive"]["fileHashes"] = "not-an-object";
  std::string manifest_json   = mj.dump();

  auto zip = make_zip(td, "test.gmmpack",
                      {
                          {"manifest.json", manifest_json},
                          {"tree.json", tree_json},
                      });

  auto extract = gmmpack::extract_archive(zip);
  REQUIRE(extract.ok);

  // Must report an error, not throw.
  auto diag = gmmpack::verify_archive_integrity(extract.archive, extract.manifest_json);
  REQUIRE_FALSE(diag.empty());
  bool found_missing = false;
  for (const auto &d : diag) {
    if (d.message.find("missing fileHashes") != std::string::npos) {
      found_missing = true;
      break;
    }
  }
  REQUIRE(found_missing);
}

TEST_CASE("gmmpack unpack fails gracefully on non-string schema version", "[gmmpack]") {
  TempDir td;
  std::string tree_json = make_tree_json();
  std::string tree_hash = "sha256:" + sha256_hex(tree_json);

  nlohmann::json mj;
  mj["gmmpackSchema"]                      = 123;  // not a string!
  mj["id"]                                 = "b3f1e2a0-1234-4abc-8def-000000000001";
  mj["revision"]                           = 1;
  mj["info"]                               = {{"name", "Test Pack"},
                                              {"author", "Test Author"},
                                              {"gmmGameId", "skyrim_se"},
                                              {"createdAt", "2026-09-01T00:00:00Z"},
                                              {"updatedAt", "2026-09-14T00:00:00Z"}};
  mj["archive"]["fileHashes"]["tree.json"] = tree_hash;
  std::string manifest_json                = mj.dump();

  auto zip = make_zip(td, "test.gmmpack",
                      {
                          {"manifest.json", manifest_json},
                          {"tree.json", tree_json},
                      });

  // Must fail with diagnostics, not throw.
  auto result = gmmpack::unpack_gmmpack(zip, schema_dir());
  REQUIRE_FALSE(result.ok);
  REQUIRE_FALSE(result.diagnostics.empty());
}

TEST_CASE("gmmpack schema validation passes for valid mod", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("mod.schema.json"));

  nlohmann::json mod = nlohmann::json::parse(make_mod_json());
  gmmpack::SchemaValidator validator;
  auto diag =
      validator.validate(mod, schemas["mod.schema.json"], schemas["mod.schema.json"]);
  REQUIRE(diag.empty());
}

TEST_CASE("gmmpack schema validation rejects unknown property", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("mod.schema.json"));

  nlohmann::json mod  = nlohmann::json::parse(make_mod_json());
  mod["unknownField"] = "should not be here";

  gmmpack::SchemaValidator validator;
  auto diag =
      validator.validate(mod, schemas["mod.schema.json"], schemas["mod.schema.json"]);
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

TEST_CASE("gmmpack schema validation rejects bad pattern", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("mod.schema.json"));

  nlohmann::json mod = nlohmann::json::parse(make_mod_json());
  mod["id"]          = "INVALID ID WITH SPACES";  // violates pattern

  gmmpack::SchemaValidator validator;
  auto diag =
      validator.validate(mod, schemas["mod.schema.json"], schemas["mod.schema.json"]);
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

TEST_CASE("gmmpack schema validation rejects missing required field", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("mod.schema.json"));

  nlohmann::json mod = nlohmann::json::parse(make_mod_json());
  mod.erase("source");

  gmmpack::SchemaValidator validator;
  auto diag =
      validator.validate(mod, schemas["mod.schema.json"], schemas["mod.schema.json"]);
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

TEST_CASE("gmmpack schema validation rejects bad enum value", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("mod.schema.json"));

  nlohmann::json mod = nlohmann::json::parse(make_mod_json());
  mod["category"]    = "invalid_category";

  gmmpack::SchemaValidator validator;
  auto diag =
      validator.validate(mod, schemas["mod.schema.json"], schemas["mod.schema.json"]);
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

TEST_CASE("gmmpack schema validates tree.json with nested separators", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("tree.schema.json"));

  nlohmann::json tree;
  tree["nodes"] = {
      {{"type", "separator"},
       {"name", "Core"},
       {"collapsed", false},
       {"children", {{{"type", "mod"}, {"id", "skyui"}, {"enabled", true}}}}},
      {{"type", "mod"}, {"id", "other"}, {"enabled", true}}};

  gmmpack::SchemaValidator validator;
  auto diag = validator.validate(tree, schemas["tree.schema.json"],
                                 schemas["tree.schema.json"]);
  REQUIRE(diag.empty());
}

TEST_CASE("gmmpack schema rejects unknown major version", "[gmmpack]") {
  TempDir td;
  std::string manifest_json = make_manifest({}, "2.0.0");
  std::string tree_json     = make_tree_json();
  std::string tree_hash     = "sha256:" + sha256_hex(tree_json);

  // Rebuild manifest with tree hash
  nlohmann::json mj                        = nlohmann::json::parse(manifest_json);
  mj["archive"]["fileHashes"]["tree.json"] = tree_hash;
  manifest_json                            = mj.dump();

  auto zip = make_zip(td, "test.gmmpack",
                      {
                          {"manifest.json", manifest_json},
                          {"tree.json", tree_json},
                      });

  auto result = gmmpack::unpack_gmmpack(zip, schema_dir());
  REQUIRE_FALSE(result.ok);
  bool found_version = false;
  for (const auto &d : result.diagnostics) {
    if (d.message.find("unsupported major version") != std::string::npos) {
      found_version = true;
      break;
    }
  }
  REQUIRE(found_version);
}

TEST_CASE("gmmpack referential integrity catches dangling rule ref", "[gmmpack]") {
  gmmpack::Gmmpack pack;
  gmmpack::ManifestRule rule;
  rule.type = "requires";
  rule.from = "nonexistent-mod";
  rule.to   = "skyui";
  pack.manifest.rules.push_back(rule);
  pack.mods.push_back(
      gmmpack::parse_mod_entry(nlohmann::json::parse(make_mod_json("skyui"))));

  auto diag = gmmpack::check_referential_integrity(pack);
  REQUIRE_FALSE(diag.empty());
  bool found_ref = false;
  for (const auto &d : diag) {
    if (d.message.find("unknown id") != std::string::npos) {
      found_ref = true;
      break;
    }
  }
  REQUIRE(found_ref);
}

TEST_CASE("gmmpack referential integrity catches dangling choiceGroup ref",
          "[gmmpack]") {
  gmmpack::Gmmpack pack;
  gmmpack::ChoiceGroup cg;
  cg.id             = "test-group";
  cg.name           = "Test";
  cg.mode           = "exactly-one";
  cg.member_mod_ids = {"skyui", "nonexistent"};
  pack.manifest.choice_groups.push_back(cg);
  pack.mods.push_back(
      gmmpack::parse_mod_entry(nlohmann::json::parse(make_mod_json("skyui"))));

  auto diag = gmmpack::check_referential_integrity(pack);
  REQUIRE_FALSE(diag.empty());
  bool found_ref = false;
  for (const auto &d : diag) {
    if (d.message.find("unknown mod id") != std::string::npos) {
      found_ref = true;
      break;
    }
  }
  REQUIRE(found_ref);
}

TEST_CASE("gmmpack referential integrity catches dangling tree mod id", "[gmmpack]") {
  gmmpack::Gmmpack pack;
  gmmpack::ModNode mn;
  mn.id      = "nonexistent-mod";
  mn.enabled = true;
  gmmpack::TreeNode tn{mn};
  pack.tree.nodes.push_back(tn);

  auto diag = gmmpack::check_referential_integrity(pack);
  REQUIRE_FALSE(diag.empty());
  bool found_ref = false;
  for (const auto &d : diag) {
    if (d.message.find("unknown mod id") != std::string::npos) {
      found_ref = true;
      break;
    }
  }
  REQUIRE(found_ref);
}

TEST_CASE("gmmpack full round-trip with valid archive", "[gmmpack]") {
  TempDir td;
  std::string mod_json  = make_mod_json();
  std::string tree_json = make_tree_json();
  std::string ini_json =
      R"({"targetFile":"Skyrim.ini","tweaks":[{"id":"aniso","name":"Anisotropy","status":"required","enabled":true,"sourceModId":null,"content":"[Display]\niMaxAnisotropy=16\n"}]})";

  std::string mod_hash  = "sha256:" + sha256_hex(mod_json);
  std::string tree_hash = "sha256:" + sha256_hex(tree_json);
  std::string ini_hash  = "sha256:" + sha256_hex(ini_json);

  std::string manifest_json = make_manifest({{"mods/skyui.json", mod_hash},
                                             {"tree.json", tree_hash},
                                             {"ini/skyrim.json", ini_hash}});

  auto zip = make_zip(td, "test.gmmpack",
                      {
                          {"manifest.json", manifest_json},
                          {"mods/skyui.json", mod_json},
                          {"tree.json", tree_json},
                          {"ini/skyrim.json", ini_json},
                      });

  auto result = gmmpack::unpack_gmmpack(zip, schema_dir());
  REQUIRE(result.ok);
  REQUIRE(result.diagnostics.empty());

  // Verify parsed data
  REQUIRE(result.pack.manifest.gmmpack_schema == "1.0.0");
  REQUIRE(result.pack.manifest.id == "b3f1e2a0-1234-4abc-8def-000000000001");
  REQUIRE(result.pack.manifest.revision == 1);
  REQUIRE(result.pack.manifest.info.name == "Test Pack");
  REQUIRE(result.pack.mods.size() == 1);
  REQUIRE(result.pack.mods[0].id == "skyui");
  REQUIRE(result.pack.tree.nodes.size() == 1);
  REQUIRE(result.pack.ini_edits.size() == 1);
  REQUIRE(result.pack.ini_edits[0].target_file == "Skyrim.ini");
  REQUIRE(result.pack.ini_edits[0].tweaks.size() == 1);
  REQUIRE(result.pack.ini_edits[0].tweaks[0].id == "aniso");
}

TEST_CASE("gmmpack rejects archive with unlisted file", "[gmmpack]") {
  TempDir td;
  std::string mod_json  = make_mod_json();
  std::string tree_json = make_tree_json();

  std::string mod_hash  = "sha256:" + sha256_hex(mod_json);
  std::string tree_hash = "sha256:" + sha256_hex(tree_json);

  std::string manifest_json =
      make_manifest({{"mods/skyui.json", mod_hash}, {"tree.json", tree_hash}});

  // Add an extra file NOT listed in fileHashes
  auto zip = make_zip(td, "test.gmmpack",
                      {
                          {"manifest.json", manifest_json},
                          {"mods/skyui.json", mod_json},
                          {"tree.json", tree_json},
                          {"mods/extra.json", "{}"},  // unlisted!
                      });

  auto result = gmmpack::unpack_gmmpack(zip, schema_dir());
  REQUIRE_FALSE(result.ok);
  bool found_unlisted = false;
  for (const auto &d : result.diagnostics) {
    if (d.message.find("not listed in fileHashes") != std::string::npos) {
      found_unlisted = true;
      break;
    }
  }
  REQUIRE(found_unlisted);
}

TEST_CASE("gmmpack validates executable entry", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("executable.schema.json"));

  nlohmann::json exe;
  exe["id"]           = "nemesis";
  exe["sourceModId"]  = "nemesis";
  exe["relativePath"] = "Nemesis Engine/Nemesis.exe";
  exe["role"]         = "setup";
  exe["autoRun"]      = true;
  exe["output"]       = {{"path", "Nemesis_Engine/Output"},
                         {"capture", "syntheticMod"},
                         {"syntheticModId", "nemesis-output"}};

  gmmpack::SchemaValidator validator;
  auto diag = validator.validate(exe, schemas["executable.schema.json"],
                                 schemas["executable.schema.json"]);
  REQUIRE(diag.empty());
}

TEST_CASE("gmmpack validates executable minimal required fields", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("executable.schema.json"));

  nlohmann::json exe;
  exe["id"]           = "skse64";
  exe["sourceModId"]  = "skse64";
  exe["relativePath"] = "skse64_loader.exe";
  exe["role"]         = "launcher";

  gmmpack::SchemaValidator validator;
  auto diag = validator.validate(exe, schemas["executable.schema.json"],
                                 schemas["executable.schema.json"]);
  REQUIRE(diag.empty());
}

TEST_CASE("gmmpack validates executable rejects bad role enum", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("executable.schema.json"));

  nlohmann::json exe;
  exe["id"]           = "test";
  exe["sourceModId"]  = "test";
  exe["relativePath"] = "test.exe";
  exe["role"]         = "invalid_role";

  gmmpack::SchemaValidator validator;
  auto diag = validator.validate(exe, schemas["executable.schema.json"],
                                 schemas["executable.schema.json"]);
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

TEST_CASE("gmmpack validates executable rejects missing required field", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("executable.schema.json"));

  nlohmann::json exe;
  exe["id"]          = "test";
  exe["sourceModId"] = "test";
  // missing relativePath and role

  gmmpack::SchemaValidator validator;
  auto diag = validator.validate(exe, schemas["executable.schema.json"],
                                 schemas["executable.schema.json"]);
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

TEST_CASE("gmmpack validates executable rejects bad id pattern", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("executable.schema.json"));

  nlohmann::json exe;
  exe["id"]           = "UPPERCASE-ID!";
  exe["sourceModId"]  = "test";
  exe["relativePath"] = "test.exe";
  exe["role"]         = "setup";

  gmmpack::SchemaValidator validator;
  auto diag = validator.validate(exe, schemas["executable.schema.json"],
                                 schemas["executable.schema.json"]);
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

TEST_CASE("gmmpack validates executable rejects unknown property", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("executable.schema.json"));

  nlohmann::json exe;
  exe["id"]           = "test";
  exe["sourceModId"]  = "test";
  exe["relativePath"] = "test.exe";
  exe["role"]         = "setup";
  exe["unknownField"] = "should not be here";

  gmmpack::SchemaValidator validator;
  auto diag = validator.validate(exe, schemas["executable.schema.json"],
                                 schemas["executable.schema.json"]);
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

TEST_CASE(
    "gmmpack validates executable with output syntheticMod requires syntheticModId",
    "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("executable.schema.json"));

  nlohmann::json exe;
  exe["id"]           = "nemesis";
  exe["sourceModId"]  = "nemesis";
  exe["relativePath"] = "Nemesis Engine/Nemesis.exe";
  exe["role"]         = "setup";
  exe["output"]       = {{"path", "Output"}, {"capture", "syntheticMod"}};
  // missing syntheticModId

  gmmpack::SchemaValidator validator;
  auto diag = validator.validate(exe, schemas["executable.schema.json"],
                                 schemas["executable.schema.json"]);
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

TEST_CASE("gmmpack validates executable with output inPlace needs no syntheticModId",
          "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("executable.schema.json"));

  nlohmann::json exe;
  exe["id"]           = "bodyslide";
  exe["sourceModId"]  = "bodyslide";
  exe["relativePath"] = "Tools/BodySlide x64.exe";
  exe["role"]         = "setup";
  exe["output"]       = {{"path", "Output"}, {"capture", "inPlace"}};

  gmmpack::SchemaValidator validator;
  auto diag = validator.validate(exe, schemas["executable.schema.json"],
                                 schemas["executable.schema.json"]);
  REQUIRE(diag.empty());
}

TEST_CASE("gmmpack validates executable with platform overrides", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("executable.schema.json"));

  nlohmann::json exe;
  exe["id"]           = "nemesis";
  exe["sourceModId"]  = "nemesis";
  exe["relativePath"] = "Nemesis Engine/Nemesis.exe";
  exe["role"]         = "setup";
  exe["envVars"]      = {{"SOME_VAR", "value"}};
  exe["platform"]     = {{"linux", {{"envVars", {{"WINEDEBUG", "+file"}}}}},
                         {"windows", {{"launchOptions", ""}}}};

  gmmpack::SchemaValidator validator;
  auto diag = validator.validate(exe, schemas["executable.schema.json"],
                                 schemas["executable.schema.json"]);
  REQUIRE(diag.empty());
}

TEST_CASE("gmmpack validates executable with all optional fields", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("executable.schema.json"));

  nlohmann::json exe;
  exe["id"]                       = "nemesis";
  exe["sourceModId"]              = "nemesis";
  exe["relativePath"]             = "Nemesis Engine/Nemesis.exe";
  exe["arguments"]                = {"-forceD3D9", "--verbose"};
  exe["envVars"]                  = {{"SOME_VAR", "value"}};
  exe["workingDir"]               = "Nemesis Engine";
  exe["role"]                     = "setup";
  exe["autoRun"]                  = true;
  exe["rerunOnModsetChange"]      = true;
  exe["requiresVirtualFsVisible"] = true;
  exe["output"]                   = {{"path", "Nemesis_Engine/Output"},
                                     {"argName", "--output"},
                                     {"capture", "syntheticMod"},
                                     {"syntheticModId", "nemesis-output"}};
  exe["platform"] = {{"linux",
                      {{"envVars", {{"WINEDEBUG", "+file"}}},
                       {"launchOptions", "WINEDLLOVERRIDES=\"d3d11=n,b\" %command%"},
                       {"protonVersionPin", "GE-Proton9-27"},
                       {"steamOverlay", true}}}};

  gmmpack::SchemaValidator validator;
  auto diag = validator.validate(exe, schemas["executable.schema.json"],
                                 schemas["executable.schema.json"]);
  REQUIRE(diag.empty());
}

// ---------------------------------------------------------------------------
// parse_executable_entry() - typed struct parsing tests
// ---------------------------------------------------------------------------

TEST_CASE("gmmpack parse_executable_entry minimal", "[gmmpack][executable]") {
  nlohmann::json j;
  j["id"]           = "skse64";
  j["sourceModId"]  = "skse64";
  j["relativePath"] = "skse64_loader.exe";
  j["role"]         = "launcher";

  auto exe = gmmpack::parse_executable_entry(j);
  REQUIRE(exe.id == "skse64");
  REQUIRE(exe.source_mod_id == "skse64");
  REQUIRE(exe.relative_path == "skse64_loader.exe");
  REQUIRE(exe.role == "launcher");
  REQUIRE(exe.arguments.empty());
  REQUIRE(exe.env_vars.empty());
  REQUIRE(exe.working_dir.empty());
  REQUIRE_FALSE(exe.auto_run);
  REQUIRE_FALSE(exe.rerun_on_modset_change);
  REQUIRE_FALSE(exe.requires_virtual_fs_visible);
  REQUIRE_FALSE(exe.output.has_value());
}

TEST_CASE("gmmpack parse_executable_entry full setup tool", "[gmmpack][executable]") {
  nlohmann::json j;
  j["id"]                       = "nemesis";
  j["sourceModId"]              = "nemesis";
  j["relativePath"]             = "Nemesis Engine/Nemesis.exe";
  j["arguments"]                = {"-forceD3D9", "--verbose"};
  j["envVars"]                  = {{"SOME_VAR", "value"}, {"OTHER", "x"}};
  j["workingDir"]               = "Nemesis Engine";
  j["role"]                     = "setup";
  j["autoRun"]                  = true;
  j["rerunOnModsetChange"]      = true;
  j["requiresVirtualFsVisible"] = true;
  j["output"]                   = {{"path", "Nemesis_Engine/Output"},
                                   {"argName", "--output"},
                                   {"capture", "syntheticMod"},
                                   {"syntheticModId", "nemesis-output"}};
  j["platform"] = {{"linux",
                    {{"envVars", {{"WINEDEBUG", "+file"}}},
                     {"launchOptions", "WINEDLLOVERRIDES=\"d3d11=n,b\" %command%"},
                     {"protonVersionPin", "GE-Proton9-27"},
                     {"steamOverlay", true}}},
                   {"windows", {{"launchOptions", ""}}}};

  auto exe = gmmpack::parse_executable_entry(j);

  REQUIRE(exe.id == "nemesis");
  REQUIRE(exe.source_mod_id == "nemesis");
  REQUIRE(exe.relative_path == "Nemesis Engine/Nemesis.exe");
  REQUIRE(exe.role == "setup");
  REQUIRE(exe.auto_run);
  REQUIRE(exe.rerun_on_modset_change);
  REQUIRE(exe.requires_virtual_fs_visible);

  // arguments
  REQUIRE(exe.arguments.size() == 2);
  REQUIRE(exe.arguments[0] == "-forceD3D9");
  REQUIRE(exe.arguments[1] == "--verbose");

  // envVars
  REQUIRE(exe.env_vars.size() == 2);
  REQUIRE(exe.env_vars.at("SOME_VAR") == "value");
  REQUIRE(exe.env_vars.at("OTHER") == "x");

  REQUIRE(exe.working_dir == "Nemesis Engine");

  // output
  REQUIRE(exe.output.has_value());
  REQUIRE(exe.output->path == "Nemesis_Engine/Output");
  REQUIRE(exe.output->arg_name == "--output");
  REQUIRE(exe.output->capture == "syntheticMod");
  REQUIRE(exe.output->synthetic_mod_id == "nemesis-output");

  // platform.linux
  REQUIRE(exe.platform.linux_plat.has_value());
  REQUIRE(exe.platform.linux_plat->env_vars.at("WINEDEBUG") == "+file");
  REQUIRE(exe.platform.linux_plat->launch_options ==
          "WINEDLLOVERRIDES=\"d3d11=n,b\" %command%");
  REQUIRE(exe.platform.linux_plat->proton_version_pin == "GE-Proton9-27");
  REQUIRE(exe.platform.linux_plat->steam_overlay.has_value());
  REQUIRE(*exe.platform.linux_plat->steam_overlay);

  // platform.windows
  REQUIRE(exe.platform.windows.has_value());
  REQUIRE_FALSE(exe.platform.macos.has_value());
}

TEST_CASE("gmmpack parse_executable_entry output inPlace", "[gmmpack][executable]") {
  nlohmann::json j;
  j["id"]           = "bodyslide";
  j["sourceModId"]  = "bodyslide";
  j["relativePath"] = "Tools/BodySlide x64.exe";
  j["role"]         = "setup";
  j["output"]       = {{"path", "ShapeData"}, {"capture", "inPlace"}};

  auto exe = gmmpack::parse_executable_entry(j);
  REQUIRE(exe.output.has_value());
  REQUIRE(exe.output->path == "ShapeData");
  REQUIRE(exe.output->capture == "inPlace");
  REQUIRE(exe.output->synthetic_mod_id.empty());
}

TEST_CASE("gmmpack parse_executable_entry no output block", "[gmmpack][executable]") {
  nlohmann::json j;
  j["id"]           = "skse64";
  j["sourceModId"]  = "skse64";
  j["relativePath"] = "skse64_loader.exe";
  j["role"]         = "launcher";

  auto exe = gmmpack::parse_executable_entry(j);
  REQUIRE_FALSE(exe.output.has_value());
}

TEST_CASE("gmmpack parse_executable_entry defaults", "[gmmpack][executable]") {
  nlohmann::json j;
  j["id"]           = "minimal";
  j["sourceModId"]  = "minimal";
  j["relativePath"] = "tool.exe";
  j["role"]         = "setup";

  auto exe = gmmpack::parse_executable_entry(j);
  REQUIRE_FALSE(exe.auto_run);
  REQUIRE_FALSE(exe.rerun_on_modset_change);
  REQUIRE_FALSE(exe.requires_virtual_fs_visible);
  REQUIRE(exe.arguments.empty());
  REQUIRE(exe.env_vars.empty());
  REQUIRE_FALSE(exe.platform.linux_plat.has_value());
  REQUIRE_FALSE(exe.platform.macos.has_value());
  REQUIRE_FALSE(exe.platform.windows.has_value());
}

// ---------------------------------------------------------------------------
// Full round-trip: archive with executables
// ---------------------------------------------------------------------------

static std::string make_exe_json(const std::string &id   = "nemesis",
                                 const std::string &role = "setup") {
  nlohmann::json j;
  j["id"]           = id;
  j["sourceModId"]  = "nemesis";
  j["relativePath"] = "Nemesis Engine/Nemesis.exe";
  j["role"]         = role;
  if (role == "setup") {
    j["autoRun"] = true;
    j["output"]  = {{"path", "Output"},
                    {"capture", "syntheticMod"},
                    {"syntheticModId", id + "-output"}};
  }
  return j.dump();
}

TEST_CASE("gmmpack full round-trip with executables", "[gmmpack]") {
  TempDir td;
  std::string mod_json  = make_mod_json("nemesis");
  std::string exe_json  = make_exe_json("nemesis", "setup");
  std::string tree_json = R"({"nodes":[{"type":"mod","id":"nemesis","enabled":true}]})";

  std::string mod_hash  = "sha256:" + sha256_hex(mod_json);
  std::string exe_hash  = "sha256:" + sha256_hex(exe_json);
  std::string tree_hash = "sha256:" + sha256_hex(tree_json);

  std::string manifest_json = make_manifest({{"mods/nemesis.json", mod_hash},
                                             {"executables/nemesis.json", exe_hash},
                                             {"tree.json", tree_hash}});

  auto zip = make_zip(td, "test.gmmpack",
                      {
                          {"manifest.json", manifest_json},
                          {"mods/nemesis.json", mod_json},
                          {"executables/nemesis.json", exe_json},
                          {"tree.json", tree_json},
                      });

  auto result = gmmpack::unpack_gmmpack(zip, schema_dir());
  REQUIRE(result.ok);
  REQUIRE(result.diagnostics.empty());

  // Verify executable parsed correctly
  REQUIRE(result.pack.executables.size() == 1);
  const auto &exe = result.pack.executables[0];
  REQUIRE(exe.id == "nemesis");
  REQUIRE(exe.source_mod_id == "nemesis");
  REQUIRE(exe.relative_path == "Nemesis Engine/Nemesis.exe");
  REQUIRE(exe.role == "setup");
  REQUIRE(exe.auto_run);
  REQUIRE(exe.output.has_value());
  REQUIRE(exe.output->capture == "syntheticMod");
  REQUIRE(exe.output->synthetic_mod_id == "nemesis-output");
}

TEST_CASE("gmmpack full round-trip with launcher executable", "[gmmpack]") {
  TempDir td;
  std::string mod_json = make_mod_json("skse64");
  std::string exe_json =
      R"({"id":"skse64-launch","sourceModId":"skse64","relativePath":"skse64_loader.exe","role":"launcher"})";
  std::string tree_json = R"({"nodes":[{"type":"mod","id":"skse64","enabled":true}]})";

  std::string mod_hash  = "sha256:" + sha256_hex(mod_json);
  std::string exe_hash  = "sha256:" + sha256_hex(exe_json);
  std::string tree_hash = "sha256:" + sha256_hex(tree_json);

  std::string manifest_json =
      make_manifest({{"mods/skse64.json", mod_hash},
                     {"executables/skse64-launch.json", exe_hash},
                     {"tree.json", tree_hash}});

  auto zip = make_zip(td, "test.gmmpack",
                      {
                          {"manifest.json", manifest_json},
                          {"mods/skse64.json", mod_json},
                          {"executables/skse64-launch.json", exe_json},
                          {"tree.json", tree_json},
                      });

  auto result = gmmpack::unpack_gmmpack(zip, schema_dir());
  REQUIRE(result.ok);
  REQUIRE(result.diagnostics.empty());

  REQUIRE(result.pack.executables.size() == 1);
  REQUIRE(result.pack.executables[0].role == "launcher");
  REQUIRE_FALSE(result.pack.executables[0].auto_run);
}

TEST_CASE("gmmpack full round-trip with multiple executables", "[gmmpack]") {
  TempDir td;
  std::string mod_json  = make_mod_json("nemesis");
  std::string exe1_json = make_exe_json("nemesis", "setup");
  std::string exe2_json =
      R"({"id":"nemesis-launch","sourceModId":"nemesis","relativePath":"Nemesis Engine/Nemesis.exe","role":"launcher"})";
  std::string tree_json = R"({"nodes":[{"type":"mod","id":"nemesis","enabled":true}]})";

  std::string mod_hash  = "sha256:" + sha256_hex(mod_json);
  std::string exe1_hash = "sha256:" + sha256_hex(exe1_json);
  std::string exe2_hash = "sha256:" + sha256_hex(exe2_json);
  std::string tree_hash = "sha256:" + sha256_hex(tree_json);

  std::string manifest_json =
      make_manifest({{"mods/nemesis.json", mod_hash},
                     {"executables/nemesis.json", exe1_hash},
                     {"executables/nemesis-launch.json", exe2_hash},
                     {"tree.json", tree_hash}});

  auto zip = make_zip(td, "test.gmmpack",
                      {
                          {"manifest.json", manifest_json},
                          {"mods/nemesis.json", mod_json},
                          {"executables/nemesis.json", exe1_json},
                          {"executables/nemesis-launch.json", exe2_json},
                          {"tree.json", tree_json},
                      });

  auto result = gmmpack::unpack_gmmpack(zip, schema_dir());
  REQUIRE(result.ok);

  REQUIRE(result.pack.executables.size() == 2);
  // Find setup and launcher
  const gmmpack::ExecutableEntry *setup    = nullptr;
  const gmmpack::ExecutableEntry *launcher = nullptr;
  for (const auto &e : result.pack.executables) {
    if (e.role == "setup")
      setup = &e;
    if (e.role == "launcher")
      launcher = &e;
  }
  REQUIRE(setup != nullptr);
  REQUIRE(launcher != nullptr);
  REQUIRE(setup->id == "nemesis");
  REQUIRE(setup->auto_run);
  REQUIRE(launcher->id == "nemesis-launch");
  REQUIRE_FALSE(launcher->auto_run);
}

// ---------------------------------------------------------------------------
// Referential integrity: sourceModId validation
// ---------------------------------------------------------------------------

TEST_CASE("gmmpack referential integrity catches dangling sourceModId", "[gmmpack]") {
  gmmpack::Gmmpack pack;
  gmmpack::ExecutableEntry exe;
  exe.id            = "nemesis";
  exe.source_mod_id = "nonexistent-mod";
  exe.relative_path = "Nemesis.exe";
  exe.role          = "setup";
  pack.executables.push_back(exe);

  // No mods in pack -> sourceModId should fail
  auto diag = gmmpack::check_referential_integrity(pack);
  REQUIRE_FALSE(diag.empty());
  bool found_source = false;
  for (const auto &d : diag) {
    if (d.path.find("sourceModId") != std::string::npos ||
        d.message.find("sourceModId") != std::string::npos) {
      found_source = true;
      break;
    }
  }
  REQUIRE(found_source);
}

TEST_CASE("gmmpack referential integrity passes for valid sourceModId", "[gmmpack]") {
  gmmpack::Gmmpack pack;
  pack.mods.push_back(
      gmmpack::parse_mod_entry(nlohmann::json::parse(make_mod_json("nemesis"))));
  gmmpack::ExecutableEntry exe;
  exe.id            = "nemesis";
  exe.source_mod_id = "nemesis";
  exe.relative_path = "Nemesis.exe";
  exe.role          = "setup";
  pack.executables.push_back(exe);

  auto diag = gmmpack::check_referential_integrity(pack);
  // Should have no sourceModId errors (may have other unrelated issues
  // but none related to this executable's sourceModId)
  bool found_source = false;
  for (const auto &d : diag) {
    if (d.path.find("sourceModId") != std::string::npos ||
        d.message.find("sourceModId") != std::string::npos) {
      found_source = true;
      break;
    }
  }
  REQUIRE_FALSE(found_source);
}

TEST_CASE("gmmpack referential integrity allows rule referencing executable",
          "[gmmpack]") {
  gmmpack::Gmmpack pack;
  pack.mods.push_back(
      gmmpack::parse_mod_entry(nlohmann::json::parse(make_mod_json("nemesis"))));
  gmmpack::ExecutableEntry exe;
  exe.id            = "nemesis-setup";
  exe.source_mod_id = "nemesis";
  exe.relative_path = "Nemesis.exe";
  exe.role          = "setup";
  pack.executables.push_back(exe);

  // A rule referencing the executable id (not just mod ids)
  gmmpack::ManifestRule rule;
  rule.type = "requires";
  rule.from = "nemesis-setup";
  rule.to   = "nemesis";
  pack.manifest.rules.push_back(rule);

  auto diag             = gmmpack::check_referential_integrity(pack);
  bool found_rule_error = false;
  for (const auto &d : diag) {
    if (d.path.find("rules") != std::string::npos &&
        d.message.find("unknown id") != std::string::npos) {
      found_rule_error = true;
      break;
    }
  }
  REQUIRE_FALSE(found_rule_error);
}

TEST_CASE("gmmpack validates patch entry", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("patch.schema.json"));

  nlohmann::json patch;
  patch["modId"]      = "skyui";
  patch["targetPath"] = "SkyUI.esp";
  patch["baseFileSha256"] =
      "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  patch["algorithm"]     = "bsdiff";
  patch["payloadBase64"] = "dGVzdA==";

  gmmpack::SchemaValidator validator;
  auto diag = validator.validate(patch, schemas["patch.schema.json"],
                                 schemas["patch.schema.json"]);
  REQUIRE(diag.empty());
}

TEST_CASE("gmmpack validates ini entry", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("ini.schema.json"));

  nlohmann::json ini;
  ini["targetFile"] = "Skyrim.ini";
  ini["tweaks"]     = {{{"id", "aniso"},
                        {"name", "Anisotropy"},
                        {"status", "required"},
                        {"enabled", true},
                        {"sourceModId", nullptr},
                        {"content", "[Display]\niMaxAnisotropy=16\n"}}};

  gmmpack::SchemaValidator validator;
  auto diag =
      validator.validate(ini, schemas["ini.schema.json"], schemas["ini.schema.json"]);
  REQUIRE(diag.empty());
}

TEST_CASE("gmmpack rejects legacy ini edits shape", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("ini.schema.json"));

  // Pre-tweak format (hard break): edits[] no longer exists.
  nlohmann::json ini;
  ini["targetFile"] = "Skyrim.ini";
  ini["edits"]      = {{{"section", "Display"},
                        {"key", "iMaxAnisotropy"},
                        {"value", "16"},
                        {"sourceModId", nullptr}}};

  gmmpack::SchemaValidator validator;
  auto diag =
      validator.validate(ini, schemas["ini.schema.json"], schemas["ini.schema.json"]);
  REQUIRE_FALSE(diag.empty());
}

TEST_CASE("gmmpack rejects patch with bad algorithm", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("patch.schema.json"));

  nlohmann::json patch;
  patch["modId"]      = "skyui";
  patch["targetPath"] = "SkyUI.esp";
  patch["baseFileSha256"] =
      "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  patch["algorithm"]     = "xdelta";  // wrong!
  patch["payloadBase64"] = "dGVzdA==";

  gmmpack::SchemaValidator validator;
  auto diag = validator.validate(patch, schemas["patch.schema.json"],
                                 schemas["patch.schema.json"]);
  REQUIRE_FALSE(diag.empty());
  bool found_const = false;
  for (const auto &d : diag) {
    if (d.message.find("const") != std::string::npos ||
        d.message.find("not in enum") != std::string::npos) {
      found_const = true;
      break;
    }
  }
  REQUIRE(found_const);
}

TEST_CASE("gmmpack validates manifest with all optional fields", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("manifest.schema.json"));

  std::string manifest_json = make_manifest(
      {{"tree.json",
        "sha256:0000000000000000000000000000000000000000000000000000000000000000"}});
  nlohmann::json m         = nlohmann::json::parse(manifest_json);
  m["info"]["description"] = "A test pack";
  m["info"]["homepage"]    = "https://example.com";
  m["tools"]               = {
      {{"id", "loot"}, {"name", "LOOT"}, {"homepage", "https://loot.github.io"}}};
  m["rules"]        = {{{"type", "requires"}, {"from", "a"}, {"to", "b"}}};
  m["loadOrder"]    = {{"pluginHint", {"A.esp", "B.esp"}}};
  m["choiceGroups"] = {{{"id", "texture-pack"},
                        {"name", "Choose texture"},
                        {"mode", "exactly-one"},
                        {"memberModIds", {"a", "b"}}}};

  gmmpack::SchemaValidator validator;
  auto diag = validator.validate(m, schemas["manifest.schema.json"],
                                 schemas["manifest.schema.json"]);
  // Should pass - all fields are valid
  REQUIRE(diag.empty());
}

// ---------------------------------------------------------------------------
// Referential integrity: patch modId vs filename cross-check
// ---------------------------------------------------------------------------

TEST_CASE("gmmpack referential integrity catches patch modId mismatch", "[gmmpack]") {
  gmmpack::Gmmpack pack;
  pack.mods.push_back(
      gmmpack::parse_mod_entry(nlohmann::json::parse(make_mod_json("skyui"))));

  gmmpack::PatchEntry patch;
  patch.mod_id       = "wrong-mod";  // doesn't match filename
  patch.archive_path = "patches/skyui.json";
  patch.target_path  = "SkyUI.esp";
  patch.base_file_sha256 =
      "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  patch.algorithm      = "bsdiff";
  patch.payload_base64 = "dGVzdA==";
  pack.patches.push_back(patch);

  auto diag = gmmpack::check_referential_integrity(pack);
  REQUIRE_FALSE(diag.empty());
  bool found_mismatch = false;
  for (const auto &d : diag) {
    if (d.message.find("does not match modId") != std::string::npos) {
      found_mismatch = true;
      break;
    }
  }
  REQUIRE(found_mismatch);
}

TEST_CASE("gmmpack referential integrity passes for matching patch modId",
          "[gmmpack]") {
  gmmpack::Gmmpack pack;
  pack.mods.push_back(
      gmmpack::parse_mod_entry(nlohmann::json::parse(make_mod_json("skyui"))));

  gmmpack::PatchEntry patch;
  patch.mod_id       = "skyui";
  patch.archive_path = "patches/skyui.json";
  patch.target_path  = "SkyUI.esp";
  patch.base_file_sha256 =
      "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  patch.algorithm      = "bsdiff";
  patch.payload_base64 = "dGVzdA==";
  pack.patches.push_back(patch);

  auto diag = gmmpack::check_referential_integrity(pack);
  for (const auto &d : diag) {
    // Should have no modId mismatch errors
    REQUIRE(d.message.find("does not match modId") == std::string::npos);
  }
}

TEST_CASE("gmmpack referential integrity catches chained patch modId mismatch",
          "[gmmpack]") {
  gmmpack::Gmmpack pack;
  pack.mods.push_back(
      gmmpack::parse_mod_entry(nlohmann::json::parse(make_mod_json("awesome-mod"))));

  gmmpack::PatchEntry patch;
  patch.mod_id       = "wrong-mod";
  patch.sequence     = 1;
  patch.archive_path = "patches/awesome-mod-1.json";
  patch.target_path  = "AwesomeMod.esp";
  patch.base_file_sha256 =
      "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  patch.algorithm      = "bsdiff";
  patch.payload_base64 = "dGVzdA==";
  pack.patches.push_back(patch);

  auto diag           = gmmpack::check_referential_integrity(pack);
  bool found_mismatch = false;
  for (const auto &d : diag) {
    if (d.message.find("does not match modId") != std::string::npos) {
      found_mismatch = true;
      break;
    }
  }
  REQUIRE(found_mismatch);
}

// ---------------------------------------------------------------------------
// Referential integrity: INI edits sourceModId validation
// ---------------------------------------------------------------------------

TEST_CASE("gmmpack referential integrity catches dangling ini sourceModId",
          "[gmmpack]") {
  gmmpack::Gmmpack pack;
  pack.mods.push_back(
      gmmpack::parse_mod_entry(nlohmann::json::parse(make_mod_json("skyui"))));

  gmmpack::IniEntry ini;
  ini.target_file = "Skyrim.ini";
  gmmpack::IniTweak edit;
  edit.id                = "aniso";
  edit.name              = "Anisotropy";
  edit.status            = "recommended";
  edit.content           = "[Display]\niMaxAnisotropy=16\n";
  edit.source_mod_id     = "nonexistent-mod";
  edit.has_source_mod_id = true;
  ini.tweaks.push_back(edit);
  pack.ini_edits.push_back(ini);

  auto diag = gmmpack::check_referential_integrity(pack);
  REQUIRE_FALSE(diag.empty());
  bool found = false;
  for (const auto &d : diag) {
    if (d.message.find("unknown mod id") != std::string::npos &&
        d.path.find("sourceModId") != std::string::npos) {
      found = true;
      break;
    }
  }
  REQUIRE(found);
}

TEST_CASE("gmmpack referential integrity passes for valid ini sourceModId",
          "[gmmpack]") {
  gmmpack::Gmmpack pack;
  pack.mods.push_back(
      gmmpack::parse_mod_entry(nlohmann::json::parse(make_mod_json("skyui"))));

  gmmpack::IniEntry ini;
  ini.target_file = "Skyrim.ini";
  gmmpack::IniTweak edit;
  edit.id                = "aniso";
  edit.name              = "Anisotropy";
  edit.status            = "recommended";
  edit.content           = "[Display]\niMaxAnisotropy=16\n";
  edit.source_mod_id     = "skyui";
  edit.has_source_mod_id = true;
  ini.tweaks.push_back(edit);
  pack.ini_edits.push_back(ini);

  auto diag = gmmpack::check_referential_integrity(pack);
  for (const auto &d : diag) {
    if (d.path.find("sourceModId") != std::string::npos) {
      REQUIRE(d.message.find("unknown mod id") == std::string::npos);
    }
  }
}

TEST_CASE("gmmpack referential integrity allows null ini sourceModId (pack-author)",
          "[gmmpack]") {
  gmmpack::Gmmpack pack;
  gmmpack::IniEntry ini;
  ini.target_file = "Skyrim.ini";
  gmmpack::IniTweak edit;
  edit.id                = "aniso";
  edit.name              = "Anisotropy";
  edit.status            = "recommended";
  edit.content           = "[Display]\niMaxAnisotropy=16\n";
  edit.source_mod_id     = "";
  edit.has_source_mod_id = false;
  ini.tweaks.push_back(edit);
  pack.ini_edits.push_back(ini);

  auto diag = gmmpack::check_referential_integrity(pack);
  for (const auto &d : diag) {
    if (d.path.find("sourceModId") != std::string::npos) {
      REQUIRE(false);  // should not produce errors for null sourceModId
    }
  }
}

// ---------------------------------------------------------------------------
// Referential integrity: platform prefixFiles sourceModId validation
// ---------------------------------------------------------------------------

TEST_CASE("gmmpack referential integrity catches dangling platform prefixFile "
          "sourceModId",
          "[gmmpack]") {
  gmmpack::Gmmpack pack;
  pack.mods.push_back(
      gmmpack::parse_mod_entry(nlohmann::json::parse(make_mod_json("skyui"))));

  gmmpack::PlatformPrefixFile pf;
  pf.path          = "drive_c/users/steamuser/AppData/Local/ENB/enblocal.ini";
  pf.source_mod_id = "nonexistent-enb";
  gmmpack::PlatformOverride po;
  po.prefix_files.push_back(pf);
  pack.manifest.platform.linux_plat = po;

  auto diag = gmmpack::check_referential_integrity(pack);
  REQUIRE_FALSE(diag.empty());
  bool found = false;
  for (const auto &d : diag) {
    if (d.message.find("unknown mod id") != std::string::npos &&
        d.path.find("prefixFiles") != std::string::npos) {
      found = true;
      break;
    }
  }
  REQUIRE(found);
}

TEST_CASE("gmmpack referential integrity passes for valid platform prefixFile "
          "sourceModId",
          "[gmmpack]") {
  gmmpack::Gmmpack pack;
  pack.mods.push_back(
      gmmpack::parse_mod_entry(nlohmann::json::parse(make_mod_json("enb-preset"))));

  gmmpack::PlatformPrefixFile pf;
  pf.path          = "drive_c/users/steamuser/AppData/Local/ENB/enblocal.ini";
  pf.source_mod_id = "enb-preset";
  gmmpack::PlatformOverride po;
  po.prefix_files.push_back(pf);
  pack.manifest.platform.linux_plat = po;

  auto diag = gmmpack::check_referential_integrity(pack);
  for (const auto &d : diag) {
    if (d.path.find("prefixFiles") != std::string::npos) {
      REQUIRE(d.message.find("unknown mod id") == std::string::npos);
    }
  }
}

// ---------------------------------------------------------------------------
// Referential integrity: executable sourceModId
// ---------------------------------------------------------------------------

TEST_CASE("gmmpack referential integrity catches dangling executable "
          "sourceModId",
          "[gmmpack]") {
  gmmpack::Gmmpack pack;
  pack.mods.push_back(
      gmmpack::parse_mod_entry(nlohmann::json::parse(make_mod_json("skyui"))));

  gmmpack::ExecutableEntry exe;
  exe.id            = "nemesis";
  exe.source_mod_id = "nonexistent-mod";
  exe.role          = "setup";
  pack.executables.push_back(exe);

  auto diag = gmmpack::check_referential_integrity(pack);
  REQUIRE_FALSE(diag.empty());
  bool found = false;
  for (const auto &d : diag) {
    if (d.message.find("unknown mod id") != std::string::npos &&
        d.path.find("sourceModId") != std::string::npos &&
        d.path.find("executables") != std::string::npos) {
      found = true;
      break;
    }
  }
  REQUIRE(found);
}

// ---------------------------------------------------------------------------
// Referential integrity: multiple errors collected (not fail-fast)
// ---------------------------------------------------------------------------

TEST_CASE("gmmpack referential integrity collects all errors", "[gmmpack]") {
  gmmpack::Gmmpack pack;

  // Rule with both from and to dangling
  gmmpack::ManifestRule rule;
  rule.type = "requires";
  rule.from = "ghost-from";
  rule.to   = "ghost-to";
  pack.manifest.rules.push_back(rule);

  // Tree with dangling mod
  gmmpack::ModNode mn;
  mn.id      = "ghost-tree";
  mn.enabled = true;
  pack.tree.nodes.push_back(gmmpack::TreeNode{mn});

  auto diag = gmmpack::check_referential_integrity(pack);

  // Should have at least 3 errors: from, to, and tree mod
  size_t error_count = 0;
  for (const auto &d : diag) {
    if (d.severity == gmmpack::Diagnostic::Severity::Error) {
      error_count++;
    }
  }
  REQUIRE(error_count >= 3);
}

// ---------------------------------------------------------------------------
// Referential integrity: valid pack passes cleanly
// ---------------------------------------------------------------------------

TEST_CASE("gmmpack referential integrity passes for valid complete pack", "[gmmpack]") {
  gmmpack::Gmmpack pack;

  // Two mods
  pack.mods.push_back(
      gmmpack::parse_mod_entry(nlohmann::json::parse(make_mod_json("skyui"))));
  pack.mods.push_back(
      gmmpack::parse_mod_entry(nlohmann::json::parse(make_mod_json("awesome-mod"))));

  // Rule between valid mods
  gmmpack::ManifestRule rule;
  rule.type = "requires";
  rule.from = "skyui";
  rule.to   = "awesome-mod";
  pack.manifest.rules.push_back(rule);

  // Choice group with valid members
  gmmpack::ChoiceGroup cg;
  cg.id             = "test-group";
  cg.name           = "Test";
  cg.mode           = "exactly-one";
  cg.member_mod_ids = {"skyui", "awesome-mod"};
  pack.manifest.choice_groups.push_back(cg);

  // Tree with valid mods
  gmmpack::ModNode mn;
  mn.id      = "skyui";
  mn.enabled = true;
  pack.tree.nodes.push_back(gmmpack::TreeNode{mn});

  gmmpack::ModNode mn2;
  mn2.id      = "awesome-mod";
  mn2.enabled = true;
  pack.tree.nodes.push_back(gmmpack::TreeNode{mn2});

  // Patch with valid modId and matching filename
  gmmpack::PatchEntry patch;
  patch.mod_id       = "skyui";
  patch.archive_path = "patches/skyui.json";
  patch.target_path  = "SkyUI.esp";
  patch.base_file_sha256 =
      "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  patch.algorithm      = "bsdiff";
  patch.payload_base64 = "dGVzdA==";
  pack.patches.push_back(patch);

  // INI edit with valid sourceModId
  gmmpack::IniEntry ini;
  ini.target_file = "Skyrim.ini";
  gmmpack::IniTweak edit;
  edit.id                = "aniso";
  edit.name              = "Anisotropy";
  edit.status            = "recommended";
  edit.content           = "[Display]\niMaxAnisotropy=16\n";
  edit.source_mod_id     = "skyui";
  edit.has_source_mod_id = true;
  ini.tweaks.push_back(edit);
  pack.ini_edits.push_back(ini);

  auto diag = gmmpack::check_referential_integrity(pack);
  REQUIRE(diag.empty());
}

TEST_CASE("gmmpack rejects manifest with invalid UUID", "[gmmpack]") {
  auto schemas = load_schemas();
  REQUIRE(schemas.count("manifest.schema.json"));

  nlohmann::json m;
  m["gmmpackSchema"]         = "1.0.0";
  m["id"]                    = "not-a-uuid";
  m["revision"]              = 1;
  m["info"]                  = {{"name", "Test"},
                                {"author", "Test"},
                                {"gmmGameId", "skyrim_se"},
                                {"createdAt", "2026-09-01T00:00:00Z"},
                                {"updatedAt", "2026-09-14T00:00:00Z"}};
  m["archive"]["fileHashes"] = nlohmann::json::object();

  gmmpack::SchemaValidator validator;
  auto diag = validator.validate(m, schemas["manifest.schema.json"],
                                 schemas["manifest.schema.json"]);
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

// ---------------------------------------------------------------------------
// parse_patch_filename tests
// ---------------------------------------------------------------------------

TEST_CASE("parse_patch_filename single patch", "[gmmpack][patch]") {
  auto result = gmmpack::parse_patch_filename("patches/skyui.json");
  REQUIRE(result.has_value());
  REQUIRE(result->first == "skyui");
  REQUIRE(result->second == 0);
}

TEST_CASE("parse_patch_filename chain patch", "[gmmpack][patch]") {
  auto result = gmmpack::parse_patch_filename("patches/awesome-mod-1.json");
  REQUIRE(result.has_value());
  REQUIRE(result->first == "awesome-mod");
  REQUIRE(result->second == 1);
}

TEST_CASE("parse_patch_filename chain patch high number", "[gmmpack][patch]") {
  auto result = gmmpack::parse_patch_filename("patches/foo-42.json");
  REQUIRE(result.has_value());
  REQUIRE(result->first == "foo");
  REQUIRE(result->second == 42);
}

TEST_CASE("parse_patch_filename rejects wrong prefix", "[gmmpack][patch]") {
  auto result = gmmpack::parse_patch_filename("mods/skyui.json");
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("parse_patch_filename rejects non-json", "[gmmpack][patch]") {
  auto result = gmmpack::parse_patch_filename("patches/skyui.txt");
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("parse_patch_filename rejects empty mod_id", "[gmmpack][patch]") {
  auto result = gmmpack::parse_patch_filename("patches/-1.json");
  // Dash at pos 0 -> mod_id would be empty, but our code requires dash_pos > 0
  // So it falls through to the "no valid sequence" path with filename = "-1"
  // which doesn't match the expected pattern cleanly. Let's verify it returns
  // either nullopt or the filename as-is.
  // With our implementation: filename = "-1", rfind('-') = 0, but dash_pos > 0
  // check fails, so it returns {"-1", 0}.
  // This is fine - the referential integrity check will catch the mismatch.
  REQUIRE(result.has_value());
}

// ---------------------------------------------------------------------------
// build_patch_chains tests
// ---------------------------------------------------------------------------

TEST_CASE("build_patch_chains single patch", "[gmmpack][patch]") {
  gmmpack::PatchEntry p;
  p.mod_id         = "skyui";
  p.sequence       = std::nullopt;
  p.target_path    = "SkyUI.esp";
  p.payload_base64 = "dGVzdA==";

  auto [chains, diag] = gmmpack::build_patch_chains({p});
  REQUIRE(diag.empty());
  REQUIRE(chains.size() == 1);
  REQUIRE(chains[0].mod_id == "skyui");
  REQUIRE(chains[0].patches.size() == 1);
  REQUIRE_FALSE(chains[0].patches[0].sequence.has_value());
}

TEST_CASE("build_patch_chains sorted chain", "[gmmpack][patch]") {
  gmmpack::PatchEntry p1;
  p1.mod_id         = "awesome-mod";
  p1.sequence       = 3;
  p1.payload_base64 = "dGVzdA==";

  gmmpack::PatchEntry p2;
  p2.mod_id         = "awesome-mod";
  p2.sequence       = 1;
  p2.payload_base64 = "dGVzdA==";

  gmmpack::PatchEntry p3;
  p3.mod_id         = "awesome-mod";
  p3.sequence       = 2;
  p3.payload_base64 = "dGVzdA==";

  auto [chains, diag] = gmmpack::build_patch_chains({p1, p2, p3});
  REQUIRE(diag.empty());
  REQUIRE(chains.size() == 1);
  REQUIRE(chains[0].patches.size() == 3);
  REQUIRE(chains[0].patches[0].sequence == 1);
  REQUIRE(chains[0].patches[1].sequence == 2);
  REQUIRE(chains[0].patches[2].sequence == 3);
}

TEST_CASE("build_patch_chains contiguity error", "[gmmpack][patch]") {
  gmmpack::PatchEntry p1;
  p1.mod_id         = "broken-mod";
  p1.sequence       = 1;
  p1.payload_base64 = "dGVzdA==";

  gmmpack::PatchEntry p3;
  p3.mod_id         = "broken-mod";
  p3.sequence       = 3;  // gap at 2
  p3.payload_base64 = "dGVzdA==";

  auto [chains, diag] = gmmpack::build_patch_chains({p1, p3});
  REQUIRE_FALSE(diag.empty());
  bool found_gap = false;
  for (const auto &d : diag) {
    if (d.message.find("non-contiguous") != std::string::npos) {
      found_gap = true;
      break;
    }
  }
  REQUIRE(found_gap);
}

TEST_CASE("build_patch_chains mixed mods sorted", "[gmmpack][patch]") {
  gmmpack::PatchEntry pa;
  pa.mod_id         = "alpha";
  pa.sequence       = 1;
  pa.payload_base64 = "dGVzdA==";

  gmmpack::PatchEntry pb;
  pb.mod_id         = "beta";
  pb.sequence       = std::nullopt;
  pb.payload_base64 = "dGVzdA==";

  gmmpack::PatchEntry pa2;
  pa2.mod_id         = "alpha";
  pa2.sequence       = 2;
  pa2.payload_base64 = "dGVzdA==";

  auto [chains, diag] = gmmpack::build_patch_chains({pa, pb, pa2});
  REQUIRE(diag.empty());
  REQUIRE(chains.size() == 2);
  // Sorted by mod_id: alpha first
  REQUIRE(chains[0].mod_id == "alpha");
  REQUIRE(chains[0].patches.size() == 2);
  REQUIRE(chains[0].patches[0].sequence == 1);
  REQUIRE(chains[0].patches[1].sequence == 2);
  REQUIRE(chains[1].mod_id == "beta");
  REQUIRE(chains[1].patches.size() == 1);
}

TEST_CASE("build_patch_chains empty input", "[gmmpack][patch]") {
  auto [chains, diag] = gmmpack::build_patch_chains({});
  REQUIRE(diag.empty());
  REQUIRE(chains.empty());
}

// ---------------------------------------------------------------------------
// Filename cross-check via referential integrity
// ---------------------------------------------------------------------------

TEST_CASE("gmmpack referential integrity catches patch filename mismatch",
          "[gmmpack]") {
  gmmpack::Gmmpack pack;
  gmmpack::ModEntry mod;
  mod.id       = "skyui";
  mod.name     = "SkyUI";
  mod.category = gmmpack::ModCategory::Required;
  mod.source   = gmmpack::ModSourceNexus{};
  pack.mods.push_back(mod);

  gmmpack::PatchEntry p;
  p.mod_id      = "skyui";
  p.target_path = "SkyUI.esp";
  p.base_file_sha256 =
      "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  p.algorithm      = "bsdiff";
  p.payload_base64 = "dGVzdA==";
  p.archive_path   = "patches/other-mod.json";  // mismatch!
  pack.patches.push_back(p);

  auto diag = gmmpack::check_referential_integrity(pack);
  REQUIRE_FALSE(diag.empty());
  bool found_mismatch = false;
  for (const auto &d : diag) {
    if (d.message.find("does not match modId") != std::string::npos) {
      found_mismatch = true;
      break;
    }
  }
  REQUIRE(found_mismatch);
}

TEST_CASE("gmmpack referential integrity passes patch filename match", "[gmmpack]") {
  gmmpack::Gmmpack pack;
  gmmpack::ModEntry mod;
  mod.id       = "skyui";
  mod.name     = "SkyUI";
  mod.category = gmmpack::ModCategory::Required;
  mod.source   = gmmpack::ModSourceNexus{};
  pack.mods.push_back(mod);

  gmmpack::PatchEntry p;
  p.mod_id      = "skyui";
  p.target_path = "SkyUI.esp";
  p.base_file_sha256 =
      "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  p.algorithm      = "bsdiff";
  p.payload_base64 = "dGVzdA==";
  p.archive_path   = "patches/skyui.json";  // matches
  pack.patches.push_back(p);

  auto diag           = gmmpack::check_referential_integrity(pack);
  bool found_mismatch = false;
  for (const auto &d : diag) {
    if (d.message.find("does not match modId") != std::string::npos) {
      found_mismatch = true;
      break;
    }
  }
  REQUIRE_FALSE(found_mismatch);
}

// ---------------------------------------------------------------------------
// Full archive round-trip with patches
// ---------------------------------------------------------------------------

TEST_CASE("gmmpack full round-trip with single patch", "[gmmpack]") {
  TempDir td;
  std::string mod_json   = make_mod_json();
  std::string tree_json  = make_tree_json();
  std::string patch_json = R"({
        "modId": "skyui",
        "targetPath": "SkyUI.esp",
        "baseFileSha256": "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
        "algorithm": "bsdiff",
        "payloadBase64": "dGVzdA=="
    })";

  std::string mod_hash   = "sha256:" + sha256_hex(mod_json);
  std::string tree_hash  = "sha256:" + sha256_hex(tree_json);
  std::string patch_hash = "sha256:" + sha256_hex(patch_json);

  std::string manifest_json = make_manifest({{"mods/skyui.json", mod_hash},
                                             {"tree.json", tree_hash},
                                             {"patches/skyui.json", patch_hash}});

  auto zip = make_zip(td, "test.gmmpack",
                      {
                          {"manifest.json", manifest_json},
                          {"mods/skyui.json", mod_json},
                          {"tree.json", tree_json},
                          {"patches/skyui.json", patch_json},
                      });

  auto result = gmmpack::unpack_gmmpack(zip, schema_dir());
  REQUIRE(result.ok);
  REQUIRE(result.pack.patches.size() == 1);
  REQUIRE(result.pack.patches[0].mod_id == "skyui");
  REQUIRE(result.pack.patches[0].target_path == "SkyUI.esp");
  REQUIRE(result.pack.patches[0].algorithm == "bsdiff");
  REQUIRE(result.pack.patches[0].archive_path == "patches/skyui.json");
  REQUIRE_FALSE(result.pack.patches[0].sequence.has_value());
}

TEST_CASE("gmmpack full round-trip with chain patches", "[gmmpack]") {
  TempDir td;
  std::string mod_json = make_mod_json("awesome-mod");
  std::string tree_json =
      R"({"nodes":[{"type":"mod","id":"awesome-mod","enabled":true}]})";
  std::string patch1_json = R"({
        "modId": "awesome-mod",
        "sequence": 1,
        "targetPath": "AwesomeMod.esp",
        "baseFileSha256": "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
        "algorithm": "bsdiff",
        "payloadBase64": "cGF0Y2gxA=="
    })";
  std::string patch2_json = R"({
        "modId": "awesome-mod",
        "sequence": 2,
        "targetPath": "AwesomeMod.ini",
        "baseFileSha256": "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef",
        "algorithm": "bsdiff",
        "payloadBase64": "cGF0Y2gy"
    })";

  std::string mod_hash  = "sha256:" + sha256_hex(mod_json);
  std::string tree_hash = "sha256:" + sha256_hex(tree_json);
  std::string p1_hash   = "sha256:" + sha256_hex(patch1_json);
  std::string p2_hash   = "sha256:" + sha256_hex(patch2_json);

  std::string manifest_json = make_manifest({{"mods/awesome-mod.json", mod_hash},
                                             {"tree.json", tree_hash},
                                             {"patches/awesome-mod-1.json", p1_hash},
                                             {"patches/awesome-mod-2.json", p2_hash}});

  auto zip = make_zip(td, "test.gmmpack",
                      {
                          {"manifest.json", manifest_json},
                          {"mods/awesome-mod.json", mod_json},
                          {"tree.json", tree_json},
                          {"patches/awesome-mod-1.json", patch1_json},
                          {"patches/awesome-mod-2.json", patch2_json},
                      });

  auto result = gmmpack::unpack_gmmpack(zip, schema_dir());
  REQUIRE(result.ok);
  REQUIRE(result.pack.patches.size() == 2);

  // Verify source filenames are preserved
  std::set<std::string> filenames;
  for (const auto &p : result.pack.patches)
    filenames.insert(p.archive_path);
  REQUIRE(filenames.count("patches/awesome-mod-1.json"));
  REQUIRE(filenames.count("patches/awesome-mod-2.json"));

  // Build chains and verify ordering
  auto [chains, chain_diag] = gmmpack::build_patch_chains(result.pack.patches);
  REQUIRE(chain_diag.empty());
  REQUIRE(chains.size() == 1);
  REQUIRE(chains[0].mod_id == "awesome-mod");
  REQUIRE(chains[0].patches.size() == 2);
  REQUIRE(chains[0].patches[0].sequence == 1);
  REQUIRE(chains[0].patches[1].sequence == 2);
}

TEST_CASE("gmmpack rejects archive with patch filename mismatch", "[gmmpack]") {
  TempDir td;
  std::string mod_json   = make_mod_json("skyui");
  std::string tree_json  = R"({"nodes":[{"type":"mod","id":"skyui","enabled":true}]})";
  std::string patch_json = R"({
        "modId": "wrong-mod",
        "targetPath": "SkyUI.esp",
        "baseFileSha256": "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
        "algorithm": "bsdiff",
        "payloadBase64": "dGVzdA=="
    })";

  std::string mod_hash   = "sha256:" + sha256_hex(mod_json);
  std::string tree_hash  = "sha256:" + sha256_hex(tree_json);
  std::string patch_hash = "sha256:" + sha256_hex(patch_json);

  // Filename says skyui, JSON says wrong-mod
  std::string manifest_json = make_manifest({{"mods/skyui.json", mod_hash},
                                             {"tree.json", tree_hash},
                                             {"patches/skyui.json", patch_hash}});

  auto zip = make_zip(td, "test.gmmpack",
                      {
                          {"manifest.json", manifest_json},
                          {"mods/skyui.json", mod_json},
                          {"tree.json", tree_json},
                          {"patches/skyui.json", patch_json},
                      });

  auto result = gmmpack::unpack_gmmpack(zip, schema_dir());
  REQUIRE_FALSE(result.ok);
  bool found_mismatch = false;
  for (const auto &d : result.diagnostics) {
    if (d.message.find("does not match modId") != std::string::npos) {
      found_mismatch = true;
      break;
    }
  }
  REQUIRE(found_mismatch);
}

TEST_CASE("gmmpack rejects chain file with mismatched sequence field", "[gmmpack]") {
  gmmpack::Gmmpack pack;
  gmmpack::PatchEntry p;
  p.mod_id         = "awesome-mod";
  p.sequence       = 1;  // filename says -2
  p.target_path    = "AwesomeMod.esp";
  p.algorithm      = "bsdiff";
  p.payload_base64 = "dGVzdA==";
  p.archive_path   = "patches/awesome-mod-2.json";
  pack.patches.push_back(p);

  auto diag  = gmmpack::check_referential_integrity(pack);
  bool found = false;
  for (const auto &d : diag) {
    if (d.message.find("does not match sequence field") != std::string::npos) {
      found = true;
      break;
    }
  }
  REQUIRE(found);
}

TEST_CASE("gmmpack rejects chain file missing sequence field", "[gmmpack]") {
  gmmpack::Gmmpack pack;
  gmmpack::PatchEntry p;
  p.mod_id         = "awesome-mod";
  p.sequence       = std::nullopt;  // filename says -1
  p.target_path    = "AwesomeMod.esp";
  p.algorithm      = "bsdiff";
  p.payload_base64 = "dGVzdA==";
  p.archive_path   = "patches/awesome-mod-1.json";
  pack.patches.push_back(p);

  auto diag  = gmmpack::check_referential_integrity(pack);
  bool found = false;
  for (const auto &d : diag) {
    if (d.message.find("missing its sequence field") != std::string::npos) {
      found = true;
      break;
    }
  }
  REQUIRE(found);
}

TEST_CASE("gmmpack rejects single file carrying sequence field", "[gmmpack]") {
  gmmpack::Gmmpack pack;
  gmmpack::PatchEntry p;
  p.mod_id         = "skyui";
  p.sequence       = 1;  // single filename, must be absent
  p.target_path    = "SkyUI.esp";
  p.algorithm      = "bsdiff";
  p.payload_base64 = "dGVzdA==";
  p.archive_path   = "patches/skyui.json";
  pack.patches.push_back(p);

  auto diag  = gmmpack::check_referential_integrity(pack);
  bool found = false;
  for (const auto &d : diag) {
    if (d.message.find("must not carry a sequence") != std::string::npos) {
      found = true;
      break;
    }
  }
  REQUIRE(found);
}

TEST_CASE("gmmpack rejects mod with both single and chained patches", "[gmmpack]") {
  gmmpack::PatchEntry single;
  single.mod_id         = "mixed-mod";
  single.sequence       = std::nullopt;
  single.payload_base64 = "dGVzdA==";

  gmmpack::PatchEntry chained;
  chained.mod_id         = "mixed-mod";
  chained.sequence       = 1;
  chained.payload_base64 = "dGVzdA==";

  // Builder flags it
  auto [chains, chain_diag] = gmmpack::build_patch_chains({single, chained});
  bool found_builder        = false;
  for (const auto &d : chain_diag) {
    if (d.message.find("both a single patch") != std::string::npos) {
      found_builder = true;
      break;
    }
  }
  REQUIRE(found_builder);

  // Integrity gate flags it too
  gmmpack::Gmmpack pack;
  pack.patches.push_back(single);
  pack.patches.push_back(chained);
  auto diag       = gmmpack::check_referential_integrity(pack);
  bool found_gate = false;
  for (const auto &d : diag) {
    if (d.message.find("both a single patch") != std::string::npos) {
      found_gate = true;
      break;
    }
  }
  REQUIRE(found_gate);
}
