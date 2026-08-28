#pragma once

#include "game_file.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::vfs {

// How component names are matched against the on-disk tree.
enum class NameCompare {
  CaseSensitive,   // Linux-native CI=false filesystems, or forced-exact callers
  CaseInsensitive, // Windows-native behavior; the default for game roots
};

// One canonical path resolver per logical root (a game dir, a mod dir, the
// overwrite dir, ...). Every subsystem that needs to turn a game-relative path
// into a real file must go through here — there is no other public way to get a
// GameFile. This is the foundation of the centralized path-resolution work:
// it owns case-insensitive matching, the identity/dedup key (normalize), and an
// incremental cache that can be invalidated when the tree changes.
//
// Platform split lives at the bottom: on Windows the OS already resolves both
// separators and case, so resolve() is a lexical normalize + exists() (no scan,
// no index). On case-sensitive platforms each component is matched
// case-insensitively against a directory_iterator and the result is cached.
class PathResolver {
public:
  explicit PathResolver(std::filesystem::path root,
                        NameCompare cmp = NameCompare::CaseInsensitive);
  ~PathResolver();

  // Movable (pimpl), not copyable: a resolver owns a cache bound to one root.
  PathResolver(PathResolver &&) noexcept;
  PathResolver &operator=(PathResolver &&) noexcept;

  // Resolve a game-relative path to its real on-disk file. Returns nullopt
  // when the path is empty, absolute, escapes via "..", or is not found.
  [[nodiscard]] std::optional<GameFile>
  resolve(std::string_view game_rel) const;

  // True when game_rel resolves to an existing file.
  [[nodiscard]] bool exists(std::string_view game_rel) const;

  // The case-insensitive identity key for game_rel. FULL variant: every
  // component including the final filename is lowercased (mirrors
  // normalize_ci_full, NOT normalize_ci_key which preserves filename case).
  // This is what callers use to dedupe / key a registry by file identity.
  [[nodiscard]] std::string normalize(std::string_view game_rel) const;

  // List the entries of a game-relative directory as GameFiles. Returns an
  // empty vector when the directory does not exist. Pass "" to list the root.
  [[nodiscard]] std::vector<GameFile> list(std::string_view dir_rel) const;

  // Drop cached knowledge about game_rel (and its containing directory).
  void invalidate(std::string_view game_rel);
  // Drop the entire cache (e.g. after a bulk mod install/remove).
  void invalidate_all();

  // True when the backend resolves case-insensitively for free because the
  // filesystem is natively CI (Windows). False on Linux/macOS where the index
  // does the work.
  [[nodiscard]] bool is_native_ci() const;

  [[nodiscard]] const std::filesystem::path &root() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  // Walk a separator-normalized, non-absolute, ".."-free relative path to its
  // real on-disk absolute path, matching each component case-insensitively and
  // caching every directory scanned. Defined in the .cpp (needs the complete
  // Impl type). Returns nullopt when a component is not found.
  [[nodiscard]] static std::optional<std::filesystem::path>
  walk_to_absolute(Impl &self, const std::string &rel);
};

} // namespace engine::vfs
