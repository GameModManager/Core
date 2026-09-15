#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "engine/gmmpack/types.h"

namespace engine::gmmpack {

// Load schema JSON files from a directory. The directory should contain
// manifest.schema.json, mod.schema.json, executable.schema.json,
// patch.schema.json, ini.schema.json, tree.schema.json.
// Returns a map of filename -> parsed JSON.
using SchemaSet = std::unordered_map<std::string, nlohmann::json>;

SchemaSet load_schema_set(const std::filesystem::path& schema_dir);

// Result of unpacking + validating a .gmmpack archive.
struct UnpackResult {
    bool ok = false;
    Diagnostics diagnostics;
    Gmmpack pack;
};

// Top-level entry point: unpack a .gmmpack archive, run all three validation
// stages, and return the parsed result.
//
// Validation order (from the spec):
//   1. Archive integrity (sha256 per file vs manifest.fileHashes)
//   2. Schema validation (JSON Schema draft 2020-12, strict)
//   3. Referential integrity (cross-file ID checks)
//
// `schema_dir` points to the directory containing the 6 schema JSON files.
// `on_progress` is optional, reports extraction progress.
UnpackResult unpack_gmmpack(const std::filesystem::path& archive_path,
                             const std::filesystem::path& schema_dir);

// Stage 1: Extract all files from the archive into memory + parse manifest.
struct ExtractResult {
    bool ok = false;
    ArchiveContents archive;
    nlohmann::json manifest_json;
    Diagnostics diagnostics;
};

ExtractResult extract_archive(const std::filesystem::path& archive_path);

// Stage 1b: Verify sha256 of every file against manifest.fileHashes.
Diagnostics verify_archive_integrity(const ArchiveContents& archive,
                                     const nlohmann::json& manifest_json);

// Stage 2: Schema-validate all JSON files against their schemas.
Diagnostics validate_schemas(const ArchiveContents& archive,
                             const nlohmann::json& manifest_json,
                             const SchemaSet& schemas);

// Stage 3: Check referential integrity (cross-file ID references).
Diagnostics check_referential_integrity(const Gmmpack& pack);

// Parse validated JSON into typed C++ structs.
Manifest parse_manifest(const nlohmann::json& j);
ModEntry parse_mod_entry(const nlohmann::json& j);
ExecutableEntry parse_executable_entry(const nlohmann::json& j);
PatchEntry parse_patch_entry(const nlohmann::json& j);
IniEntry parse_ini_entry(const nlohmann::json& j);
TreeRoot parse_tree(const nlohmann::json& j);

}  // namespace engine::gmmpack
