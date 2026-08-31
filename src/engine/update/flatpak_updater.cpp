#include "engine/update/flatpak_updater.h"

#ifdef __linux__

#include <cstdlib>
#include <filesystem>

#include "engine/core/log/logger.h"
#include "engine/update/self_updater_p.h"

namespace engine::update {

std::unique_ptr<SelfUpdater> create_flatpak_updater() {
  return std::make_unique<FlatpakUpdater>();
}

UpdateInfo FlatpakUpdater::check_for_update() {
  // Flatpak manages its own updates; we still query GitHub for version
  // comparison so the UI can display "update available" with a changelog.
  UpdateInfo info = fetch_update_info("");
  return info;
}

InstallResult FlatpakUpdater::install_update(
    const UpdateInfo &info, std::function<void(float progress)> progress_cb) {
  InstallResult result;

  if (!info.available) {
    result.error_message = "No update available.";
    return result;
  }

  if (progress_cb)
    progress_cb(0.0f);

  // Use flatpak update with the app ID from the FLATPAK_ID env var.
  const char *app_id = std::getenv("FLATPAK_ID");
  if (!app_id || app_id[0] == '\0') {
    result.error_message = "FLATPAK_ID environment variable not set.";
    return result;
  }

  std::string cmd =
      "flatpak update --assumeyes " + std::string(app_id) + " 2>&1";
  int rc = std::system(cmd.c_str());

  if (progress_cb)
    progress_cb(1.0f);

  if (rc != 0) {
    result.error_message =
        "flatpak update failed (exit " + std::to_string(rc) + ")";
    return result;
  }

  result.success = true;
  result.requires_restart = true;
  return result;
}

void FlatpakUpdater::restart() {
  const char *app_id = std::getenv("FLATPAK_ID");
  if (app_id && app_id[0] != '\0') {
    std::string cmd = "flatpak run " + std::string(app_id) + " &";
    std::system(cmd.c_str());
  }
  std::exit(0);
}

} // namespace engine::update

#endif // __linux__
