#pragma once

#include "engine/deploy/deploy_utils.h"
#include "engine/deploy/interface.h"

#include <filesystem>

namespace Deploy {

// Symlink deploy strategy: creates symlinks from game_dir back to mod folders.
// Executables and scripts are copied as real files (not symlinked) so sibling
// path resolution works correctly.
class Symlink : public Interface {
public:
  // case_sensitive=false routes targets through resolve_deploy_target_ci for
  // games whose filesystem is case-insensitive (Windows games), so CI-equal
  // directory paths merge into one on-disk casing.
  explicit Symlink(bool case_sensitive = true);

  bool deploy(const std::filesystem::path &source,
              const std::filesystem::path &target) override;
  bool remove(const std::filesystem::path &target) override;

private:
  bool case_sensitive_;
};

} // namespace Deploy
