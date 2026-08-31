#pragma once

#include "engine/update/self_updater.h"

namespace engine::update {

// Generic Linux updater - fallback for non-store/non-package installs.
// Downloads a tarball or binary from GitHub and replaces in-place.
class LinuxSelfUpdater : public SelfUpdater {
public:
  UpdateInfo check_for_update() override;
  InstallResult
  install_update(const UpdateInfo &info,
                 std::function<void(float progress)> progress_cb) override;
  void restart() override;
};

} // namespace engine::update
