#pragma once

#include <filesystem>
#include <string>

namespace engine::vfs {

class PathResolver;

// A resolved on-disk file. Constructed only by PathResolver, so every instance
// is guaranteed to have come through the single canonical resolution seam - raw
// game-relative filesystem access is structurally impossible once callers adopt
// this type. The three string/path views describe the same file from three
// angles:
//   - absolute()   : the real on-disk path, with the tree's actual casing.
//   - normalized() : the case-insensitive identity key (see
//   PathResolver::normalize).
//   - logical()    : the original game-relative spelling the caller asked
//   about.
class GameFile {
public:
  [[nodiscard]] const std::filesystem::path &absolute() const {
    return absolute_;
  }
  [[nodiscard]] const std::string &normalized() const { return normalized_; }
  [[nodiscard]] const std::string &logical() const { return logical_; }

  // True when the file still exists on disk at absolute(). Implemented as a
  // real stat (no cached flag) so a GameFile stays meaningful even if the
  // underlying file is removed after resolution.
  [[nodiscard]] bool exists() const {
    std::error_code ec;
    return std::filesystem::exists(absolute_, ec);
  }

private:
  friend class PathResolver;
  GameFile(std::filesystem::path abs, std::string norm, std::string logical)
      : absolute_(std::move(abs)), normalized_(std::move(norm)),
        logical_(std::move(logical)) {}

  std::filesystem::path absolute_;
  std::string normalized_;
  std::string logical_;
};

} // namespace engine::vfs
