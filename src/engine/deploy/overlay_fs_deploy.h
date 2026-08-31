#pragma once

#include "engine/deploy/interface.h"

#include <filesystem>
#include <vector>

namespace Deploy {

// OverlayFS deploy strategy: mod files are symlinked into a staging directory
// (not game_dir). At launch time, the staging directory is layered on top of
// game_dir via OverlayFS, capturing all writes to the overwrite directory.
// game_dir is NEVER touched - no symlinks, no writes.
class OverlayFsDeploy : public Interface {
public:
  explicit OverlayFsDeploy(std::filesystem::path staging_dir,
                           bool case_sensitive = true);

  bool deploy(const std::filesystem::path &source,
              const std::filesystem::path &target) override;
  bool remove(const std::filesystem::path &target) override;

  void set_mod_paths(const std::vector<std::filesystem::path> &paths);
  const std::vector<std::filesystem::path> &mod_paths() const;
  const std::filesystem::path &staging_dir() const { return staging_dir_; }

private:
  std::filesystem::path staging_dir_;
  std::vector<std::filesystem::path> mod_paths_;
  bool case_sensitive_;
};

} // namespace Deploy
