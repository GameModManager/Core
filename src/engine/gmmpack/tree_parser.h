#pragma once

// Engine-layer utilities for working with the gmmpack tree.json structure.
//
// The tree.json file encodes a recursive node hierarchy: separator nodes
// (name, collapsed, children) and mod nodes (id, enabled). Separators may
// nest. The flattened top-to-bottom sequence of mod nodes IS the
// file-conflict-resolution priority order (MO2-style), not merely cosmetic.
//
// parse_tree() converts tree.json JSON to the typed structs. It lives here
// (pure JSON, no archive I/O) so engine and test code can use the tree
// module without pulling in unpacker.h's libarchive/OpenSSL dependencies.
// unpacker.h re-declares it for backward compatibility.

#include "engine/gmmpack/types.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace engine::gmmpack {

// ---------------------------------------------------------------------------
// Flatten
// ---------------------------------------------------------------------------

// One entry in the flattened mod sequence. Each entry records the mod id,
// its enabled flag, and the separator path from root to this node (empty
// vector = top-level mod). The vector order IS the conflict-resolution
// priority -- index 0 wins over index 1, etc.
struct FlattenedMod {
  std::string id;
  bool enabled = true;
  std::vector<std::string> separator_path;  // e.g. {"Core", "ENB"}
};

// Parse tree.json JSON into the typed structs (recursive: separators nest,
// mod nodes carry id + enabled). Missing "nodes" yields an empty tree.
TreeRoot parse_tree(const nlohmann::json &j);

std::vector<FlattenedMod> flatten_tree(const TreeRoot &tree);

// ---------------------------------------------------------------------------
// Count
// ---------------------------------------------------------------------------

// Total number of mod nodes (recursively) in the tree.
std::size_t count_tree_mods(const TreeRoot &tree);

// Total number of separator nodes (recursively) in the tree.
std::size_t count_tree_separators(const TreeRoot &tree);

// ---------------------------------------------------------------------------
// Serialize
// ---------------------------------------------------------------------------

// Serialize a TreeRoot back to tree.json-compatible JSON.
nlohmann::json serialize_tree(const TreeRoot &tree);

// ---------------------------------------------------------------------------
// Validate
// ---------------------------------------------------------------------------

// Validate the tree structure standalone (no Gmmpack context needed).
// Checks:
//   - All nodes are valid variant alternatives (separator or mod)
//   - Separator names are non-empty
//   - Mod ids are non-empty
//   - No duplicate mod ids anywhere in the tree
// Returns diagnostics (empty = valid).
Diagnostics validate_tree(const TreeRoot &tree);

}  // namespace engine::gmmpack
