#pragma once

// Source-agnostic collection sync/tracking (Workspace-5wmu).
//
// Tracks which installed mods belong to which collection revision and diffs
// a newly fetched revision against the installed set on update. Works for
// ANY collection format: it only reads Manifest::id/revision, ModEntry::id,
// and the per-source version string - never provider-specific fields.
//
// Membership lives on engine::Mod (collection_id, collection_revision,
// in_collection) and persists via ModMeta's [GameModManager] keys. The
// Resolver stamps those fields during resolve(); these helpers tag/untag
// manually and diff.
//
// Engine layer - Qt-free.

#include "engine/collection/manifest.h"
#include "engine/mod/model/mod.h"

#include <cstdint>
#include <string>
#include <vector>

namespace engine::Collection {

// ---------------------------------------------------------------------------
// Revision comparison
// ---------------------------------------------------------------------------

enum class RevisionRelation {
  Older,  // incoming revision < installed revision (rollback / stale fetch)
  Same,   // revisions match - nothing to sync
  Newer,  // incoming revision > installed revision (update available)
};

RevisionRelation compare_revision(int64_t installed, int64_t incoming);

// ---------------------------------------------------------------------------
// Membership helpers (operate on engine::Mod tracking fields)
// ---------------------------------------------------------------------------

// Mark a mod as belonging to a collection revision.
void tag(::engine::Mod &mod, const std::string &collection_id, int64_t revision);

// Remove collection membership (standalone mod again).
void untag(::engine::Mod &mod);

// True when the mod is tracked as part of the given collection.
bool belongs_to(const ::engine::Mod &mod, const std::string &collection_id);

// ---------------------------------------------------------------------------
// Diff
// ---------------------------------------------------------------------------

// Per-mod outcome of diffing a new revision against the installed set.
struct CollectionDiff {
  std::vector<std::string> added;      // in incoming, not installed
  std::vector<std::string> removed;    // installed+tracked, not in incoming
  std::vector<std::string> updated;    // in both, version changed
  std::vector<std::string> unchanged;  // in both, version matches
};

// Source-agnostic version of a manifest entry: every ModSource variant
// carries a version string (Steam Workshop reports it unknown/empty).
std::string entry_version(const ModEntry &entry);

// Diff an incoming manifest revision against installed mods.
//
// Only installed mods tracked to incoming.id (in_collection + matching
// collection_id) participate: untracked standalone mods and mods belonging
// to other collections are ignored. Updated = version string differs
// between the installed mod and the incoming entry.
CollectionDiff diff_revision(const std::vector<::engine::Mod> &installed,
                             const Manifest &incoming);

}  // namespace engine::Collection
