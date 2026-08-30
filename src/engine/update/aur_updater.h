#pragma once

#include "engine/update/self_updater.h"

namespace engine::update {

// AUR updater — uses yay or paru to update the pacman-installed package.
class AurUpdater : public SelfUpdater {
public:
  UpdateInfo check_for_update() override;
  InstallResult
  install_update(const UpdateInfo &info,
                 std::function<void(float progress)> progress_cb) override;
  void restart() override;
};

} // namespace engine::update
