#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "engine/modpack/gmmpack/types.h"

namespace engine::gmmpack {

// Load schema JSON files from a directory. The directory should contain
// manifest.schema.json, mod.schema.json, executable.schema.json,
// patch.schema.json, ini.schema.json, tree.schema.json.
// Returns a map of filename -> parsed JSON.
using SchemaSet = std::unordered_map<std::string, nlohmann::json>;

SchemaSet load_schema_set(const std::filesystem::path &schema_dir);

// Locate the schema directory for an installed or dev-launched binary:
// app_dir/schemas, <app_dir>/../share/gamemodmanager/schemas, then the
// schemas/ dir of any ancestor of app_dir (the source tree). Empty when none
// of them has manifest.schema.json. Qt-free: callers pass the app dir in.
std::filesystem::path find_schema_dir(const std::filesystem::path &app_dir);

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
UnpackResult unpack_gmmpack(const std::filesystem::path &archive_path,
                            const std::filesystem::path &schema_dir);

// Stage 1: Extract all files from the archive into memory + parse manifest.
struct ExtractResult {
  bool ok = false;
  ArchiveContents archive;
  nlohmann::json manifest_json;
  Diagnostics diagnostics;
};

ExtractResult extract_archive(const std::filesystem::path &archive_path);

// Stage 1b: Verify sha256 of every file against manifest.fileHashes.
Diagnostics verify_archive_integrity(const ArchiveContents &archive,
                                     const nlohmann::json &manifest_json);

// Stage 2: Schema-validate all JSON files against their schemas.
Diagnostics validate_schemas(const ArchiveContents &archive,
                             const nlohmann::json &manifest_json,
                             const SchemaSet &schemas);

// Stage 3: Check referential integrity (cross-file ID references).
Diagnostics check_referential_integrity(const Gmmpack &pack);

// Parse validated JSON into typed C++ structs.
// (parse_ini_entry lives in ini_edit_parser.h: the thin parse-only layer.)
Manifest parse_manifest(const nlohmann::json &j);
ModEntry parse_mod_entry(const nlohmann::json &j);
ExecutableEntry parse_executable_entry(const nlohmann::json &j);
PatchEntry parse_patch_entry(const nlohmann::json &j);
TreeRoot parse_tree(const nlohmann::json &j);

// Mod lookup by id. Null when the pack has no such mod.
const ModEntry *find_mod(const Gmmpack &pack, const std::string &mod_id);

// Write an embedded mod's files/** payload into dest_dir. Every file's size
// and sha256 are verified against the mod entry BEFORE anything is written, so
// a tampered archive fails without leaving a half-extracted mod behind.
// Fails when the mod is not an embedded source or its payload is missing.
bool extract_embedded_mod(const Gmmpack &pack, const std::string &mod_id,
                          const std::filesystem::path &dest_dir, std::string &error);

// ---------------------------------------------------------------------------
// Patch filename parsing and chain building
// ---------------------------------------------------------------------------

// Extract mod_id and optional sequence from a patch archive path.
// Valid patterns: "patches/<mod_id>.json" (single) or "patches/<mod_id>-<N>.json"
// (chain). Returns nullopt if the path doesn't match the expected pattern.
std::optional<std::pair<std::string, int>>
parse_patch_filename(const std::string &path);

// Group patches by mod_id, sort each group by sequence ascending,
// and validate contiguity (1, 2, 3...). Single patches (no sequence)
// become a chain of one. A mod with both single and chained patches is
// an error. Chains are always returned alongside diagnostics - callers
// must refuse to apply when diagnostics contain errors.
std::pair<std::vector<PatchChain>, Diagnostics>
build_patch_chains(const std::vector<PatchEntry> &patches);

}  // namespace engine::gmmpack
