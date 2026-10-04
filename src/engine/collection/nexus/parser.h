#pragma once

// Nexus Mods collection.json parser.
//
// Parses the Vortex-compatible collection.json format (as exported by Nexus
// Mods) and maps it to the source-agnostic Collection::Manifest interface.
// This is the adapter from Nexus format to the internal format.
//
// Engine layer - Qt-free. Depends on nlohmann/json for parsing.

#include "engine/collection/manifest.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace engine::Collection::Nexus {

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

struct ParseError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

// Parse a Nexus collection.json string into a Collection::Manifest.
//
// The JSON format is the one produced by Vortex's collection export:
//   { "info": {...}, "mods": [...], "modRules": [...] }
//
// Throws ParseError on malformed or semantically invalid JSON.
Manifest parse(std::string_view json);

// Convenience overload: parse from a file path.
Manifest parse_file(const std::string &path);

// Merge a collection.json - the one inside the revision's downloaded .zip - over
// a Manifest built from metadata, with the archive authoritative for everything
// it alone carries. Throws ParseError if the JSON is unreadable, so the caller
// can treat a broken archive the same way it treats an absent one.
//
// The archive contributes, and the metadata query cannot:
//   info.installInstructions, info.gameVersions
//   loadOrder
//   mods[].hashes[]     - the per-file {path, md5} identity, the only one Nexus
//                         publishes for files inside a mod's archive
//   mods[].instructions - the collection author's per-mod note
//   mods[].choices      - the nested FOMOD block
//   source.md5, source.tag, source.fileSize
//   modRules, re-bound to the manifest's own mod ids
//
// A mod is matched across the two by its Nexus mod id, so an id the manifest
// already published is preserved and existing references to it stay valid; a mod
// only the archive lists is appended. Nothing already in the manifest is
// dropped, and nothing the archive declared is dropped silently: what could not
// be bound lands in Manifest::unresolved.
void merge_collection_json(Manifest &manifest, std::string_view json);

// ---------------------------------------------------------------------------
// Update policy
// ---------------------------------------------------------------------------

// Map Nexus's updatePolicy ("exact" | "prefer" | "latest") to our three
// states. Never yields Exact: Nexus publishes an md5 of the archive and never a
// sha256, and an exact pin is a claim about a digest we would have to invent
// to make it true. So "exact" keeps its fileId pin but degrades to Prefer -
// same file while it is still there, the mod's newest file once it is not -
// and an absent or unrecognised value is treated the same way, since nothing
// was actually declared. Shared by the collection.json parser and the live
// revision adapter so both read Nexus's enum the same way.
UpdatePolicy map_update_policy(std::string_view nexus_policy);

}  // namespace engine::Collection::Nexus
