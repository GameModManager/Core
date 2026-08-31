#pragma once

#include "engine/update/self_updater.h"

namespace engine::update {

// Windows updater - handles .exe/.msi installer-based updates.
class WindowsSelfUpdater : public SelfUpdater {
public:
  UpdateInfo check_for_update() override;
  InstallResult
  install_update(const UpdateInfo &info,
                 std::function<void(float progress)> progress_cb) override;
  void restart() override;
};

} // namespace engine::update
