#include "engine/update/aur_updater.h"

#ifdef __linux__

#include <cstdlib>
#include <filesystem>

#include "engine/core/log/logger.h"
#include "engine/update/self_updater_p.h"

namespace engine::update {

namespace {

// Find the AUR helper (yay or paru) on PATH.
std::string find_aur_helper() {
  int rc = std::system("which yay >/dev/null 2>&1");
  if (rc == 0)
    return "yay";
  rc = std::system("which paru >/dev/null 2>&1");
  if (rc == 0)
    return "paru";
  return {};
}

} // namespace

std::unique_ptr<SelfUpdater> create_aur_updater() {
  return std::make_unique<AurUpdater>();
}

UpdateInfo AurUpdater::check_for_update() {
  // Check GitHub for version comparison and changelog, since the AUR
  // PKGBUILD tracks the upstream release.
  return fetch_update_info("");
}

InstallResult
AurUpdater::install_update(const UpdateInfo &info,
                           std::function<void(float progress)> progress_cb) {
  InstallResult result;

  if (!info.available) {
    result.error_message = "No update available.";
    return result;
  }

  const std::string helper = find_aur_helper();
  if (helper.empty()) {
    result.error_message = "No AUR helper found (install yay or paru).";
    return result;
  }

  if (progress_cb)
    progress_cb(0.0f);

  // -Ss to search, -S to install. Use --noconfirm for non-interactive.
  std::string cmd = helper + " -S gamemodmanager --noconfirm 2>&1";
  int rc = std::system(cmd.c_str());

  if (progress_cb)
    progress_cb(1.0f);

  if (rc != 0) {
    result.error_message =
        helper + " install failed (exit " + std::to_string(rc) + ")";
    return result;
  }

  result.success = true;
  result.requires_restart = true;
  return result;
}

void AurUpdater::restart() {
  const auto exe = std::filesystem::current_path() / "gamemodmanager";
  if (std::filesystem::exists(exe)) {
    std::string cmd = "\"" + exe.string() + "\" &";
    std::system(cmd.c_str());
  }
  std::exit(0);
}

} // namespace engine::update

#endif // __linux__
