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
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;
namespace gmmpack = engine::gmmpack;

// ---------------------------------------------------------------------------
// SHA-256 helper (matches unpacker.cpp)
// ---------------------------------------------------------------------------

static std::string sha256_hex(const std::string& data) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
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
        root = fs::temp_directory_path() /
               ("gmmpack_test_" + std::to_string(::getpid()) + "_" +
                std::to_string(counter++));
        fs::create_directories(root);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

static fs::path make_zip(const TempDir& td, const std::string& name,
                          const std::vector<ZipItem>& items) {
    auto path = td.root / name;
    struct archive* a = archive_write_new();
    archive_write_set_format_zip(a);
    archive_write_set_options(a, "zip:compression=deflate");
    REQUIRE(archive_write_open_filename(a, path.string().c_str()) ==
            ARCHIVE_OK);
    for (const auto& item : items) {
        struct archive_entry* e = archive_entry_new();
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
    auto candidate = project_root / ".." / ".." / "input";
    if (fs::is_directory(candidate)) return fs::canonical(candidate);
    candidate = project_root / ".." / "input";
    if (fs::is_directory(candidate)) return fs::canonical(candidate);
    FAIL("schema dir not found from " + project_root.string());
    return candidate;
}

static gmmpack::SchemaSet load_schemas() {
    auto dir = schema_dir();
    auto schemas = gmmpack::load_schema_set(dir);
    INFO("schema_dir=" << dir.string() << " count=" << schemas.size());
    return schemas;
}

// ---------------------------------------------------------------------------
// Minimal valid manifest.json
// ---------------------------------------------------------------------------

static std::string make_manifest(
    const std::unordered_map<std::string, std::string>& file_hashes,
    const std::string& schema_version = "1.0.0") {
    nlohmann::json j;
    j["gmmpackSchema"] = schema_version;
    j["id"] = "b3f1e2a0-1234-4abc-8def-000000000001";
    j["revision"] = 1;
    j["info"] = {{"name", "Test Pack"},
                 {"author", "Test Author"},
                 {"gmmGameId", "skyrim_se"},
                 {"createdAt", "2026-09-01T00:00:00Z"},
                 {"updatedAt", "2026-09-14T00:00:00Z"}};
    j["archive"]["fileHashes"] = file_hashes;
    return j.dump();
}

static std::string make_manifest_no_hash() {
    nlohmann::json j;
    j["gmmpackSchema"] = "1.0.0";
    j["id"] = "b3f1e2a0-1234-4abc-8def-000000000001";
    j["revision"] = 1;
    j["info"] = {{"name", "Test Pack"},
                 {"author", "Test Author"},
                 {"gmmGameId", "skyrim_se"},
                 {"createdAt", "2026-09-01T00:00:00Z"},
                 {"updatedAt", "2026-09-14T00:00:00Z"}};
    j["archive"]["fileHashes"] = nlohmann::json::object();
    return j.dump();
}

// A valid mod entry
static std::string make_mod_json(const std::string& id = "skyui") {
    nlohmann::json j;
    j["id"] = id;
    j["name"] = "SkyUI";
    j["category"] = "required";
    j["source"] = {{"provider", "nexus"},
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
    std::string mod_json = make_mod_json();
    std::string tree_json = make_tree_json();

    // Compute hashes
    std::string mod_hash = "sha256:" + sha256_hex(mod_json);
    std::string tree_hash = "sha256:" + sha256_hex(tree_json);

    std::string manifest_json =
        make_manifest({{"mods/skyui.json", mod_hash},
                       {"tree.json", tree_hash}});

    auto zip = make_zip(td, "test.gmmpack", {
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
    auto zip = make_zip(td, "test.gmmpack", {
        {"mods/skyui.json", "{}"},
    });
    auto result = gmmpack::extract_archive(zip);
    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(result.diagnostics.empty());
}

TEST_CASE("gmmpack archive integrity passes", "[gmmpack]") {
    TempDir td;
    std::string mod_json = make_mod_json();
    std::string tree_json = make_tree_json();

    std::string mod_hash = "sha256:" + sha256_hex(mod_json);
    std::string tree_hash = "sha256:" + sha256_hex(tree_json);
    std::string manifest_json =
        make_manifest({{"mods/skyui.json", mod_hash},
                       {"tree.json", tree_hash}});

    auto zip = make_zip(td, "test.gmmpack", {
        {"manifest.json", manifest_json},
        {"mods/skyui.json", mod_json},
        {"tree.json", tree_json},
    });

    auto extract = gmmpack::extract_archive(zip);
    REQUIRE(extract.ok);

    auto diag = gmmpack::verify_archive_integrity(
        extract.archive, extract.manifest_json);
    REQUIRE(diag.empty());
}

TEST_CASE("gmmpack archive integrity fails on hash mismatch", "[gmmpack]") {
    TempDir td;
    std::string mod_json = make_mod_json();
    std::string tree_json = make_tree_json();

    // Wrong hash
    std::string mod_hash = "sha256:0000000000000000000000000000000000000000000000000000000000000000";
    std::string tree_hash = "sha256:" + sha256_hex(tree_json);
    std::string manifest_json =
        make_manifest({{"mods/skyui.json", mod_hash},
                       {"tree.json", tree_hash}});

    auto zip = make_zip(td, "test.gmmpack", {
        {"manifest.json", manifest_json},
        {"mods/skyui.json", mod_json},
        {"tree.json", tree_json},
    });

    auto extract = gmmpack::extract_archive(zip);
    REQUIRE(extract.ok);

    auto diag = gmmpack::verify_archive_integrity(
        extract.archive, extract.manifest_json);
    REQUIRE_FALSE(diag.empty());
    bool found_mismatch = false;
    for (const auto& d : diag) {
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
    std::string tree_hash = "sha256:" + sha256_hex(tree_json);
    std::string manifest_json =
        make_manifest({{"mods/skyui.json",
                        "sha256:0000000000000000000000000000000000000000000000000000000000000000"},
                       {"tree.json", tree_hash}});

    auto zip = make_zip(td, "test.gmmpack", {
        {"manifest.json", manifest_json},
        {"tree.json", tree_json},
        // mods/skyui.json is missing!
    });

    auto extract = gmmpack::extract_archive(zip);
    REQUIRE(extract.ok);

    auto diag = gmmpack::verify_archive_integrity(
        extract.archive, extract.manifest_json);
    REQUIRE_FALSE(diag.empty());
    bool found_missing = false;
    for (const auto& d : diag) {
        if (d.message.find("not in archive") != std::string::npos) {
            found_missing = true;
            break;
        }
    }
    REQUIRE(found_missing);
}

TEST_CASE("gmmpack schema validation passes for valid mod", "[gmmpack]") {
    auto schemas = load_schemas();
    REQUIRE(schemas.count("mod.schema.json"));

    nlohmann::json mod = nlohmann::json::parse(make_mod_json());
    gmmpack::SchemaValidator validator;
    auto diag = validator.validate(mod, schemas["mod.schema.json"],
                                   schemas["mod.schema.json"]);
    REQUIRE(diag.empty());
}

TEST_CASE("gmmpack schema validation rejects unknown property",
          "[gmmpack]") {
    auto schemas = load_schemas();
    REQUIRE(schemas.count("mod.schema.json"));

    nlohmann::json mod = nlohmann::json::parse(make_mod_json());
    mod["unknownField"] = "should not be here";

    gmmpack::SchemaValidator validator;
    auto diag = validator.validate(mod, schemas["mod.schema.json"],
                                   schemas["mod.schema.json"]);
    REQUIRE_FALSE(diag.empty());
    bool found_unknown = false;
    for (const auto& d : diag) {
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
    mod["id"] = "INVALID ID WITH SPACES";  // violates pattern

    gmmpack::SchemaValidator validator;
    auto diag = validator.validate(mod, schemas["mod.schema.json"],
                                   schemas["mod.schema.json"]);
    REQUIRE_FALSE(diag.empty());
    bool found_pattern = false;
    for (const auto& d : diag) {
        if (d.message.find("pattern") != std::string::npos ||
            d.message.find("does not match") != std::string::npos) {
            found_pattern = true;
            break;
        }
    }
    REQUIRE(found_pattern);
}

TEST_CASE("gmmpack schema validation rejects missing required field",
          "[gmmpack]") {
    auto schemas = load_schemas();
    REQUIRE(schemas.count("mod.schema.json"));

    nlohmann::json mod = nlohmann::json::parse(make_mod_json());
    mod.erase("source");

    gmmpack::SchemaValidator validator;
    auto diag = validator.validate(mod, schemas["mod.schema.json"],
                                   schemas["mod.schema.json"]);
    REQUIRE_FALSE(diag.empty());
    bool found_required = false;
    for (const auto& d : diag) {
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
    mod["category"] = "invalid_category";

    gmmpack::SchemaValidator validator;
    auto diag = validator.validate(mod, schemas["mod.schema.json"],
                                   schemas["mod.schema.json"]);
    REQUIRE_FALSE(diag.empty());
    bool found_enum = false;
    for (const auto& d : diag) {
        if (d.message.find("not in enum") != std::string::npos) {
            found_enum = true;
            break;
        }
    }
    REQUIRE(found_enum);
}

TEST_CASE("gmmpack schema validates tree.json with nested separators",
          "[gmmpack]") {
    auto schemas = load_schemas();
    REQUIRE(schemas.count("tree.schema.json"));

    nlohmann::json tree;
    tree["nodes"] = {{{"type", "separator"},
                       {"name", "Core"},
                       {"collapsed", false},
                       {"children", {{{"type", "mod"},
                                       {"id", "skyui"},
                                       {"enabled", true}}}}},
                      {{"type", "mod"}, {"id", "other"}, {"enabled", true}}};

    gmmpack::SchemaValidator validator;
    auto diag = validator.validate(tree, schemas["tree.schema.json"],
                                   schemas["tree.schema.json"]);
    REQUIRE(diag.empty());
}

TEST_CASE("gmmpack schema rejects unknown major version", "[gmmpack]") {
    TempDir td;
    std::string manifest_json = make_manifest({}, "2.0.0");
    std::string tree_json = make_tree_json();
    std::string tree_hash = "sha256:" + sha256_hex(tree_json);

    // Rebuild manifest with tree hash
    nlohmann::json mj = nlohmann::json::parse(manifest_json);
    mj["archive"]["fileHashes"]["tree.json"] = tree_hash;
    manifest_json = mj.dump();

    auto zip = make_zip(td, "test.gmmpack", {
        {"manifest.json", manifest_json},
        {"tree.json", tree_json},
    });

    auto result = gmmpack::unpack_gmmpack(zip, schema_dir());
    REQUIRE_FALSE(result.ok);
    bool found_version = false;
    for (const auto& d : result.diagnostics) {
        if (d.message.find("unsupported major version") !=
            std::string::npos) {
            found_version = true;
            break;
        }
    }
    REQUIRE(found_version);
}

TEST_CASE("gmmpack referential integrity catches dangling rule ref",
          "[gmmpack]") {
    gmmpack::Gmmpack pack;
    gmmpack::ManifestRule rule;
    rule.type = "requires";
    rule.from = "nonexistent-mod";
    rule.to = "skyui";
    pack.manifest.rules.push_back(rule);
    pack.mods.push_back(gmmpack::parse_mod_entry(
        nlohmann::json::parse(make_mod_json("skyui"))));

    auto diag = gmmpack::check_referential_integrity(pack);
    REQUIRE_FALSE(diag.empty());
    bool found_ref = false;
    for (const auto& d : diag) {
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
    cg.id = "test-group";
    cg.name = "Test";
    cg.mode = "exactly-one";
    cg.member_mod_ids = {"skyui", "nonexistent"};
    pack.manifest.choice_groups.push_back(cg);
    pack.mods.push_back(gmmpack::parse_mod_entry(
        nlohmann::json::parse(make_mod_json("skyui"))));

    auto diag = gmmpack::check_referential_integrity(pack);
    REQUIRE_FALSE(diag.empty());
    bool found_ref = false;
    for (const auto& d : diag) {
        if (d.message.find("unknown mod id") != std::string::npos) {
            found_ref = true;
            break;
        }
    }
    REQUIRE(found_ref);
}

TEST_CASE("gmmpack referential integrity catches dangling tree mod id",
          "[gmmpack]") {
    gmmpack::Gmmpack pack;
    gmmpack::ModNode mn;
    mn.id = "nonexistent-mod";
    mn.enabled = true;
    gmmpack::TreeNode tn{mn};
    pack.tree.nodes.push_back(tn);

    auto diag = gmmpack::check_referential_integrity(pack);
    REQUIRE_FALSE(diag.empty());
    bool found_ref = false;
    for (const auto& d : diag) {
        if (d.message.find("unknown mod id") != std::string::npos) {
            found_ref = true;
            break;
        }
    }
    REQUIRE(found_ref);
}

TEST_CASE("gmmpack full round-trip with valid archive", "[gmmpack]") {
    TempDir td;
    std::string mod_json = make_mod_json();
    std::string tree_json = make_tree_json();
    std::string ini_json =
        R"({"targetFile":"Skyrim.ini","edits":[{"section":"Display","key":"iMaxAnisotropy","value":"16","sourceModId":null}]})";

    std::string mod_hash = "sha256:" + sha256_hex(mod_json);
    std::string tree_hash = "sha256:" + sha256_hex(tree_json);
    std::string ini_hash = "sha256:" + sha256_hex(ini_json);

    std::string manifest_json =
        make_manifest({{"mods/skyui.json", mod_hash},
                       {"tree.json", tree_hash},
                       {"ini/skyrim.json", ini_hash}});

    auto zip = make_zip(td, "test.gmmpack", {
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
    REQUIRE(result.pack.manifest.id ==
            "b3f1e2a0-1234-4abc-8def-000000000001");
    REQUIRE(result.pack.manifest.revision == 1);
    REQUIRE(result.pack.manifest.info.name == "Test Pack");
    REQUIRE(result.pack.mods.size() == 1);
    REQUIRE(result.pack.mods[0].id == "skyui");
    REQUIRE(result.pack.tree.nodes.size() == 1);
    REQUIRE(result.pack.ini_edits.size() == 1);
    REQUIRE(result.pack.ini_edits[0].target_file == "Skyrim.ini");
}

TEST_CASE("gmmpack rejects archive with unlisted file", "[gmmpack]") {
    TempDir td;
    std::string mod_json = make_mod_json();
    std::string tree_json = make_tree_json();

    std::string mod_hash = "sha256:" + sha256_hex(mod_json);
    std::string tree_hash = "sha256:" + sha256_hex(tree_json);

    std::string manifest_json =
        make_manifest({{"mods/skyui.json", mod_hash},
                       {"tree.json", tree_hash}});

    // Add an extra file NOT listed in fileHashes
    auto zip = make_zip(td, "test.gmmpack", {
        {"manifest.json", manifest_json},
        {"mods/skyui.json", mod_json},
        {"tree.json", tree_json},
        {"mods/extra.json", "{}"},  // unlisted!
    });

    auto result = gmmpack::unpack_gmmpack(zip, schema_dir());
    REQUIRE_FALSE(result.ok);
    bool found_unlisted = false;
    for (const auto& d : result.diagnostics) {
        if (d.message.find("not listed in fileHashes") !=
            std::string::npos) {
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
    exe["id"] = "nemesis";
    exe["sourceModId"] = "nemesis";
    exe["relativePath"] = "Nemesis Engine/Nemesis.exe";
    exe["role"] = "setup";
    exe["autoRun"] = true;
    exe["output"] = {{"path", "Nemesis_Engine/Output"},
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
    exe["id"] = "skse64";
    exe["sourceModId"] = "skse64";
    exe["relativePath"] = "skse64_loader.exe";
    exe["role"] = "launcher";

    gmmpack::SchemaValidator validator;
    auto diag = validator.validate(exe, schemas["executable.schema.json"],
                                   schemas["executable.schema.json"]);
    REQUIRE(diag.empty());
}

TEST_CASE("gmmpack validates executable rejects bad role enum", "[gmmpack]") {
    auto schemas = load_schemas();
    REQUIRE(schemas.count("executable.schema.json"));

    nlohmann::json exe;
    exe["id"] = "test";
    exe["sourceModId"] = "test";
    exe["relativePath"] = "test.exe";
    exe["role"] = "invalid_role";

    gmmpack::SchemaValidator validator;
    auto diag = validator.validate(exe, schemas["executable.schema.json"],
                                   schemas["executable.schema.json"]);
    REQUIRE_FALSE(diag.empty());
    bool found_enum = false;
    for (const auto& d : diag) {
        if (d.message.find("not in enum") != std::string::npos) {
            found_enum = true;
            break;
        }
    }
    REQUIRE(found_enum);
}

TEST_CASE("gmmpack validates executable rejects missing required field",
          "[gmmpack]") {
    auto schemas = load_schemas();
    REQUIRE(schemas.count("executable.schema.json"));

    nlohmann::json exe;
    exe["id"] = "test";
    exe["sourceModId"] = "test";
    // missing relativePath and role

    gmmpack::SchemaValidator validator;
    auto diag = validator.validate(exe, schemas["executable.schema.json"],
                                   schemas["executable.schema.json"]);
    REQUIRE_FALSE(diag.empty());
    bool found_required = false;
    for (const auto& d : diag) {
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
    exe["id"] = "UPPERCASE-ID!";
    exe["sourceModId"] = "test";
    exe["relativePath"] = "test.exe";
    exe["role"] = "setup";

    gmmpack::SchemaValidator validator;
    auto diag = validator.validate(exe, schemas["executable.schema.json"],
                                   schemas["executable.schema.json"]);
    REQUIRE_FALSE(diag.empty());
    bool found_pattern = false;
    for (const auto& d : diag) {
        if (d.message.find("pattern") != std::string::npos ||
            d.message.find("does not match") != std::string::npos) {
            found_pattern = true;
            break;
        }
    }
    REQUIRE(found_pattern);
}

TEST_CASE("gmmpack validates executable rejects unknown property",
          "[gmmpack]") {
    auto schemas = load_schemas();
    REQUIRE(schemas.count("executable.schema.json"));

    nlohmann::json exe;
    exe["id"] = "test";
    exe["sourceModId"] = "test";
    exe["relativePath"] = "test.exe";
    exe["role"] = "setup";
    exe["unknownField"] = "should not be here";

    gmmpack::SchemaValidator validator;
    auto diag = validator.validate(exe, schemas["executable.schema.json"],
                                   schemas["executable.schema.json"]);
    REQUIRE_FALSE(diag.empty());
    bool found_unknown = false;
    for (const auto& d : diag) {
        if (d.message.find("unknown property") != std::string::npos) {
            found_unknown = true;
            break;
        }
    }
    REQUIRE(found_unknown);
}

TEST_CASE("gmmpack validates executable with output syntheticMod requires syntheticModId",
          "[gmmpack]") {
    auto schemas = load_schemas();
    REQUIRE(schemas.count("executable.schema.json"));

    nlohmann::json exe;
    exe["id"] = "nemesis";
    exe["sourceModId"] = "nemesis";
    exe["relativePath"] = "Nemesis Engine/Nemesis.exe";
    exe["role"] = "setup";
    exe["output"] = {{"path", "Output"},
                     {"capture", "syntheticMod"}};
    // missing syntheticModId

    gmmpack::SchemaValidator validator;
    auto diag = validator.validate(exe, schemas["executable.schema.json"],
                                   schemas["executable.schema.json"]);
    REQUIRE_FALSE(diag.empty());
    bool found_required = false;
    for (const auto& d : diag) {
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
    exe["id"] = "bodyslide";
    exe["sourceModId"] = "bodyslide";
    exe["relativePath"] = "Tools/BodySlide x64.exe";
    exe["role"] = "setup";
    exe["output"] = {{"path", "Output"},
                     {"capture", "inPlace"}};

    gmmpack::SchemaValidator validator;
    auto diag = validator.validate(exe, schemas["executable.schema.json"],
                                   schemas["executable.schema.json"]);
    REQUIRE(diag.empty());
}

TEST_CASE("gmmpack validates executable with platform overrides", "[gmmpack]") {
    auto schemas = load_schemas();
    REQUIRE(schemas.count("executable.schema.json"));

    nlohmann::json exe;
    exe["id"] = "nemesis";
    exe["sourceModId"] = "nemesis";
    exe["relativePath"] = "Nemesis Engine/Nemesis.exe";
    exe["role"] = "setup";
    exe["envVars"] = {{"SOME_VAR", "value"}};
    exe["platform"] = {
        {"linux", {{"envVars", {{"WINEDEBUG", "+file"}}}}},
        {"windows", {{"launchOptions", ""}}}
    };

    gmmpack::SchemaValidator validator;
    auto diag = validator.validate(exe, schemas["executable.schema.json"],
                                   schemas["executable.schema.json"]);
    REQUIRE(diag.empty());
}

TEST_CASE("gmmpack validates executable with all optional fields", "[gmmpack]") {
    auto schemas = load_schemas();
    REQUIRE(schemas.count("executable.schema.json"));

    nlohmann::json exe;
    exe["id"] = "nemesis";
    exe["sourceModId"] = "nemesis";
    exe["relativePath"] = "Nemesis Engine/Nemesis.exe";
    exe["arguments"] = {"-forceD3D9", "--verbose"};
    exe["envVars"] = {{"SOME_VAR", "value"}};
    exe["workingDir"] = "Nemesis Engine";
    exe["role"] = "setup";
    exe["autoRun"] = true;
    exe["rerunOnModsetChange"] = true;
    exe["requiresVirtualFsVisible"] = true;
    exe["output"] = {{"path", "Nemesis_Engine/Output"},
                     {"argName", "--output"},
                     {"capture", "syntheticMod"},
                     {"syntheticModId", "nemesis-output"}};
    exe["platform"] = {
        {"linux", {{"envVars", {{"WINEDEBUG", "+file"}}},
                   {"launchOptions", "WINEDLLOVERRIDES=\"d3d11=n,b\" %command%"},
                   {"protonVersionPin", "GE-Proton9-27"},
                   {"steamOverlay", true}}}
    };

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
    j["id"] = "skse64";
    j["sourceModId"] = "skse64";
    j["relativePath"] = "skse64_loader.exe";
    j["role"] = "launcher";

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
    j["id"] = "nemesis";
    j["sourceModId"] = "nemesis";
    j["relativePath"] = "Nemesis Engine/Nemesis.exe";
    j["arguments"] = {"-forceD3D9", "--verbose"};
    j["envVars"] = {{"SOME_VAR", "value"}, {"OTHER", "x"}};
    j["workingDir"] = "Nemesis Engine";
    j["role"] = "setup";
    j["autoRun"] = true;
    j["rerunOnModsetChange"] = true;
    j["requiresVirtualFsVisible"] = true;
    j["output"] = {{"path", "Nemesis_Engine/Output"},
                   {"argName", "--output"},
                   {"capture", "syntheticMod"},
                   {"syntheticModId", "nemesis-output"}};
    j["platform"] = {
        {"linux", {{"envVars", {{"WINEDEBUG", "+file"}}},
                   {"launchOptions", "WINEDLLOVERRIDES=\"d3d11=n,b\" %command%"},
                   {"protonVersionPin", "GE-Proton9-27"},
                   {"steamOverlay", true}}},
        {"windows", {{"launchOptions", ""}}}
    };

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
    j["id"] = "bodyslide";
    j["sourceModId"] = "bodyslide";
    j["relativePath"] = "Tools/BodySlide x64.exe";
    j["role"] = "setup";
    j["output"] = {{"path", "ShapeData"},
                   {"capture", "inPlace"}};

    auto exe = gmmpack::parse_executable_entry(j);
    REQUIRE(exe.output.has_value());
    REQUIRE(exe.output->path == "ShapeData");
    REQUIRE(exe.output->capture == "inPlace");
    REQUIRE(exe.output->synthetic_mod_id.empty());
}

TEST_CASE("gmmpack parse_executable_entry no output block", "[gmmpack][executable]") {
    nlohmann::json j;
    j["id"] = "skse64";
    j["sourceModId"] = "skse64";
    j["relativePath"] = "skse64_loader.exe";
    j["role"] = "launcher";

    auto exe = gmmpack::parse_executable_entry(j);
    REQUIRE_FALSE(exe.output.has_value());
}

TEST_CASE("gmmpack parse_executable_entry defaults", "[gmmpack][executable]") {
    nlohmann::json j;
    j["id"] = "minimal";
    j["sourceModId"] = "minimal";
    j["relativePath"] = "tool.exe";
    j["role"] = "setup";

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

static std::string make_exe_json(const std::string& id = "nemesis",
                                 const std::string& role = "setup") {
    nlohmann::json j;
    j["id"] = id;
    j["sourceModId"] = "nemesis";
    j["relativePath"] = "Nemesis Engine/Nemesis.exe";
    j["role"] = role;
    if (role == "setup") {
        j["autoRun"] = true;
        j["output"] = {{"path", "Output"},
                       {"capture", "syntheticMod"},
                       {"syntheticModId", id + "-output"}};
    }
    return j.dump();
}

TEST_CASE("gmmpack full round-trip with executables", "[gmmpack]") {
    TempDir td;
    std::string mod_json = make_mod_json("nemesis");
    std::string exe_json = make_exe_json("nemesis", "setup");
    std::string tree_json =
        R"({"nodes":[{"type":"mod","id":"nemesis","enabled":true}]})";

    std::string mod_hash = "sha256:" + sha256_hex(mod_json);
    std::string exe_hash = "sha256:" + sha256_hex(exe_json);
    std::string tree_hash = "sha256:" + sha256_hex(tree_json);

    std::string manifest_json = make_manifest(
        {{"mods/nemesis.json", mod_hash},
         {"executables/nemesis.json", exe_hash},
         {"tree.json", tree_hash}});

    auto zip = make_zip(td, "test.gmmpack", {
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
    const auto& exe = result.pack.executables[0];
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
    std::string tree_json =
        R"({"nodes":[{"type":"mod","id":"skse64","enabled":true}]})";

    std::string mod_hash = "sha256:" + sha256_hex(mod_json);
    std::string exe_hash = "sha256:" + sha256_hex(exe_json);
    std::string tree_hash = "sha256:" + sha256_hex(tree_json);

    std::string manifest_json = make_manifest(
        {{"mods/skse64.json", mod_hash},
         {"executables/skse64-launch.json", exe_hash},
         {"tree.json", tree_hash}});

    auto zip = make_zip(td, "test.gmmpack", {
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
    std::string mod_json = make_mod_json("nemesis");
    std::string exe1_json = make_exe_json("nemesis", "setup");
    std::string exe2_json =
        R"({"id":"nemesis-launch","sourceModId":"nemesis","relativePath":"Nemesis Engine/Nemesis.exe","role":"launcher"})";
    std::string tree_json =
        R"({"nodes":[{"type":"mod","id":"nemesis","enabled":true}]})";

    std::string mod_hash = "sha256:" + sha256_hex(mod_json);
    std::string exe1_hash = "sha256:" + sha256_hex(exe1_json);
    std::string exe2_hash = "sha256:" + sha256_hex(exe2_json);
    std::string tree_hash = "sha256:" + sha256_hex(tree_json);

    std::string manifest_json = make_manifest(
        {{"mods/nemesis.json", mod_hash},
         {"executables/nemesis.json", exe1_hash},
         {"executables/nemesis-launch.json", exe2_hash},
         {"tree.json", tree_hash}});

    auto zip = make_zip(td, "test.gmmpack", {
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
    const gmmpack::ExecutableEntry* setup = nullptr;
    const gmmpack::ExecutableEntry* launcher = nullptr;
    for (const auto& e : result.pack.executables) {
        if (e.role == "setup") setup = &e;
        if (e.role == "launcher") launcher = &e;
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

TEST_CASE("gmmpack referential integrity catches dangling sourceModId",
          "[gmmpack]") {
    gmmpack::Gmmpack pack;
    gmmpack::ExecutableEntry exe;
    exe.id = "nemesis";
    exe.source_mod_id = "nonexistent-mod";
    exe.relative_path = "Nemesis.exe";
    exe.role = "setup";
    pack.executables.push_back(exe);

    // No mods in pack -> sourceModId should fail
    auto diag = gmmpack::check_referential_integrity(pack);
    REQUIRE_FALSE(diag.empty());
    bool found_source = false;
    for (const auto& d : diag) {
        if (d.path.find("sourceModId") != std::string::npos ||
            d.message.find("sourceModId") != std::string::npos) {
            found_source = true;
            break;
        }
    }
    REQUIRE(found_source);
}

TEST_CASE("gmmpack referential integrity passes for valid sourceModId",
          "[gmmpack]") {
    gmmpack::Gmmpack pack;
    pack.mods.push_back(gmmpack::parse_mod_entry(
        nlohmann::json::parse(make_mod_json("nemesis"))));
    gmmpack::ExecutableEntry exe;
    exe.id = "nemesis";
    exe.source_mod_id = "nemesis";
    exe.relative_path = "Nemesis.exe";
    exe.role = "setup";
    pack.executables.push_back(exe);

    auto diag = gmmpack::check_referential_integrity(pack);
    // Should have no sourceModId errors (may have other unrelated issues
    // but none related to this executable's sourceModId)
    bool found_source = false;
    for (const auto& d : diag) {
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
    pack.mods.push_back(gmmpack::parse_mod_entry(
        nlohmann::json::parse(make_mod_json("nemesis"))));
    gmmpack::ExecutableEntry exe;
    exe.id = "nemesis-setup";
    exe.source_mod_id = "nemesis";
    exe.relative_path = "Nemesis.exe";
    exe.role = "setup";
    pack.executables.push_back(exe);

    // A rule referencing the executable id (not just mod ids)
    gmmpack::ManifestRule rule;
    rule.type = "requires";
    rule.from = "nemesis-setup";
    rule.to = "nemesis";
    pack.manifest.rules.push_back(rule);

    auto diag = gmmpack::check_referential_integrity(pack);
    bool found_rule_error = false;
    for (const auto& d : diag) {
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
    patch["modId"] = "skyui";
    patch["targetPath"] = "SkyUI.esp";
    patch["baseFileSha256"] =
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    patch["algorithm"] = "bsdiff";
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
    ini["edits"] = {{{"section", "Display"},
                      {"key", "iMaxAnisotropy"},
                      {"value", "16"},
                      {"sourceModId", nullptr}}};

    gmmpack::SchemaValidator validator;
    auto diag = validator.validate(ini, schemas["ini.schema.json"],
                                   schemas["ini.schema.json"]);
    REQUIRE(diag.empty());
}

TEST_CASE("gmmpack rejects patch with bad algorithm", "[gmmpack]") {
    auto schemas = load_schemas();
    REQUIRE(schemas.count("patch.schema.json"));

    nlohmann::json patch;
    patch["modId"] = "skyui";
    patch["targetPath"] = "SkyUI.esp";
    patch["baseFileSha256"] =
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    patch["algorithm"] = "xdelta";  // wrong!
    patch["payloadBase64"] = "dGVzdA==";

    gmmpack::SchemaValidator validator;
    auto diag = validator.validate(patch, schemas["patch.schema.json"],
                                   schemas["patch.schema.json"]);
    REQUIRE_FALSE(diag.empty());
    bool found_const = false;
    for (const auto& d : diag) {
        if (d.message.find("const") != std::string::npos ||
            d.message.find("not in enum") != std::string::npos) {
            found_const = true;
            break;
        }
    }
    REQUIRE(found_const);
}

TEST_CASE("gmmpack validates manifest with all optional fields",
          "[gmmpack]") {
    auto schemas = load_schemas();
    REQUIRE(schemas.count("manifest.schema.json"));

    std::string manifest_json = make_manifest(
        {{"tree.json", "sha256:0000000000000000000000000000000000000000000000000000000000000000"}});
    nlohmann::json m = nlohmann::json::parse(manifest_json);
    m["info"]["description"] = "A test pack";
    m["info"]["homepage"] = "https://example.com";
    m["tools"] = {{{"id", "loot"}, {"name", "LOOT"}, {"homepage", "https://loot.github.io"}}};
    m["rules"] = {{{"type", "requires"}, {"from", "a"}, {"to", "b"}}};
    m["loadOrder"] = {{"pluginHint", {"A.esp", "B.esp"}}};
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

TEST_CASE("gmmpack rejects manifest with invalid UUID", "[gmmpack]") {
    auto schemas = load_schemas();
    REQUIRE(schemas.count("manifest.schema.json"));

    nlohmann::json m;
    m["gmmpackSchema"] = "1.0.0";
    m["id"] = "not-a-uuid";
    m["revision"] = 1;
    m["info"] = {{"name", "Test"},
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
    for (const auto& d : diag) {
        if (d.message.find("UUID") != std::string::npos) {
            found_uuid = true;
            break;
        }
    }
    REQUIRE(found_uuid);
}
