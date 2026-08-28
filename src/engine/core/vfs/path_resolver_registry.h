#pragma once

#include "engine/core/vfs/path_resolver.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace engine::vfs {

// Process-wide cache of PathResolver instances, one per logical root (a game
// dir, a mod dir, the staging dir, the overwrite dir, ...). Wired to the
// EventBus so mod/profile changes drop the stale on-disk indexes:
// PathResolver's index is a cache derived from the deploy ledger / on-disk
// tree, never the authority (the ledger stays the source of truth). Qt-free: it
// includes EventBus directly (EventBus is also Qt-free) rather than
// PathResolver depending on it, satisfying the engine/core/vfs "no Qt" rule.
//
// Resolvers are keyed by (root path, NameCompare) so a case-sensitive game and
// a case-insensitive game at the same root get distinct indexes. References
// returned by resolver() stay valid for the process lifetime (entries are
// never erased, only their caches invalidated), so callers may hold a
// PathResolver& across a single operation.
class PathResolverRegistry {
public:
  static PathResolverRegistry &instance();

  // Returns the resolver for (root, cmp), creating it on first request.
  [[nodiscard]] PathResolver &
  resolver(const std::filesystem::path &root,
           NameCompare cmp = NameCompare::CaseInsensitive);

  // Drop every cached resolver's index (e.g. after a bulk mod install/remove).
  void invalidate_all();

private:
  PathResolverRegistry();

  std::map<std::string, PathResolver> resolvers_;
  std::vector<uint64_t> subs_;
};

} // namespace engine::vfs
