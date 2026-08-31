#pragma once

#include "engine/update/self_updater.h"

namespace engine::update {

// Debian/RPM updater — downloads and installs .deb or .rpm packages.
class DebRpmUpdater : public SelfUpdater {
public:
  // pkg_type should be "deb" or "rpm".
  explicit DebRpmUpdater(std::string pkg_type);

  UpdateInfo check_for_update() override;
  InstallResult
  install_update(const UpdateInfo &info,
                 std::function<void(float progress)> progress_cb) override;
  void restart() override;

private:
  std::string pkg_type_; // "deb" or "rpm"
};

} // namespace engine::update
