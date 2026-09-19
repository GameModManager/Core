#pragma once

// Modpack export (write side of .gmmpack). Reads an InstanceSnapshot plus
// each mod's in-folder meta.ini and produces a schema-valid Gmmpack struct
// (and, via create_gmmpack, a .gmmpack archive on disk).
//
// Mirror of unpacker.h: the serialize_* helpers here emit exactly the
// camelCase field names the parse_* functions in unpacker.cpp read.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/core/instance/instance_snapshot.h"
#include "engine/gmmpack/types.h"
#include "engine/mod/meta/mod_meta.h"

namespace engine::gmmpack
{

struct PackOptions
{
  std::string author;
  std::string description;
  std::string homepage;
  std::string instructions;
};

struct PackResult
{
  bool ok = false;
  std::string error;
  std::filesystem::path output_path;
};

// Map a mod's meta.ini to its pack source. Nullopt for manual/unknown
// sources (those mods are skipped on export - the pack format has no
// manual provider). game_id is the GMM game id (nexus gameDomain fallback);
// steam_appid feeds the steam_workshop appId field.
std::optional<ModSource> resolve_mod_source(const ModMeta& meta,
                                            const std::string& game_id,
                                            uint32_t steam_appid = 0);

// Folder name -> schema-valid mod id slug. Lowercase alnum runs joined by
// single hyphens; collisions get -2/-3 suffixes. Deterministic for a given
// input set (inputs are sorted before slugging).
std::string mod_slug(const std::string& folder_name);

// Build the display tree from mod-state nesting. Separators are entries
// whose folder name appears as another entry's parent_separator; children
// sort by list_position. ModNode enabled = !hidden && !disabled. Manual/
// unresolvable mods are omitted (they have no mods/<id>.json to point at).
TreeRoot build_tree(const InstanceSnapshot& snapshot,
                    const std::filesystem::path& mods_dir);

// Fresh manifest: UUID v4 id, schema "1.0.0", revision 1, info from
// snapshot + options, current UTC timestamps. archive.fileHashes is left
// empty - write_gmmpack_archive fills it after serializing every file.
Manifest build_manifest(const InstanceSnapshot& snapshot, const PackOptions& options);

// One ModEntry per snapshot mod with a resolvable source. Manual/unknown
// sources are skipped. Order is deterministic (list_position, then folder).
std::vector<ModEntry> build_mod_entries(const InstanceSnapshot& snapshot,
                                        const std::filesystem::path& mods_dir);

// Snapshot executables -> pack executables. Entries whose mod does not
// resolve to an exported mod (game-root exes, manual mods) are skipped:
// sourceModId must pass referential integrity.
std::vector<ExecutableEntry> build_executables(const InstanceSnapshot& snapshot,
                                               const std::filesystem::path& mods_dir);

// JSON serializers (reverse of unpacker.cpp parse_*).
nlohmann::json serialize_manifest(const Manifest& m);
nlohmann::json serialize_mod_source(const ModSource& source);
nlohmann::json serialize_mod_entry(const ModEntry& m);
nlohmann::json serialize_executable_entry(const ExecutableEntry& e);

// Assemble the full pack in memory.
Gmmpack build_gmmpack(const InstanceSnapshot& snapshot,
                      const std::filesystem::path& mods_dir,
                      const PackOptions& options);

// Serialize + write a .gmmpack (zip) archive: manifest.json, tree.json,
// mods/*.json, executables/*.json, instructions.md when present.
// fileHashes are computed over the serialized payloads and baked into the
// manifest before writing, so the archive passes verify_archive_integrity.
PackResult create_gmmpack(const InstanceSnapshot& snapshot,
                          const std::filesystem::path& mods_dir,
                          const PackOptions& options,
                          const std::filesystem::path& output_path);

}  // namespace engine::gmmpack
