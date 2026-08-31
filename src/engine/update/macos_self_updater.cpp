#include "engine/update/macos_self_updater.h"

#ifdef __APPLE__

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "engine/core/log/logger.h"
#include "engine/source/download/curl_download.h"
#include "engine/update/self_updater_p.h"

namespace engine::update {

std::unique_ptr<SelfUpdater> create_macos_updater() {
  return std::make_unique<MacOSSelfUpdater>();
}

UpdateInfo MacOSSelfUpdater::check_for_update() {
  return fetch_update_info(".dmg");
}

InstallResult MacOSSelfUpdater::install_update(
    const UpdateInfo &info, std::function<void(float progress)> progress_cb) {
  InstallResult result;

  if (!info.available || info.download_url.empty()) {
    result.error_message = "No update available or download URL missing.";
    return result;
  }

  // Download the DMG.
  const auto dmg_path = std::filesystem::temp_directory_path() / "gmm.dmg";

  if (progress_cb)
    progress_cb(0.0f);

  namespace dl = engine::Source::DownloadManager;
  long http_code = 0;
  dl::Options opts;
  opts.user_agent = "GameModManager/SelfUpdater";

  dl::Progress dl_progress;
  dl_progress.callback = [&progress_cb](int64_t current, int64_t total,
                                        double /*speed*/) {
    if (total > 0 && progress_cb) {
      progress_cb(static_cast<float>(current) / static_cast<float>(total));
    }
  };
  dl_progress.start = std::chrono::steady_clock::now();

  bool ok = dl::curl_download(info.download_url, dmg_path, http_code, opts,
                              &dl_progress);
  if (!ok || http_code >= 400) {
    result.error_message =
        "Download failed (HTTP " + std::to_string(http_code) + ")";
    return result;
  }

  if (progress_cb)
    progress_cb(1.0f);

  // Mount the DMG, copy the .app, unmount.
  const std::string mount_point = "/Volumes/GameModManagerUpdate";

  // hdiutil attach -nobrowse -mountpoint <mp> <dmg>
  std::string attach_cmd = "hdiutil attach -nobrowse -mountpoint \"" +
                           mount_point + "\" \"" + dmg_path.string() + "\"";
  int rc = std::system(attach_cmd.c_str());
  if (rc != 0) {
    result.error_message =
        "Failed to mount DMG (hdiutil exit " + std::to_string(rc) + ")";
    return result;
  }

  // Find the .app inside the mounted volume.
  std::filesystem::path app_src;
  auto it = std::find_if(
      std::filesystem::directory_iterator(mount_point),
      std::filesystem::directory_iterator{},
      [](const auto &entry) {
        return entry.path().extension() == ".app";
      });
  if (it != std::filesystem::directory_iterator{}) {
    app_src = it->path();
  }

  if (app_src.empty()) {
    result.error_message = "No .app found in mounted DMG.";
    std::system(("hdiutil detach \"" + mount_point + "\"").c_str());
    return result;
  }

  const std::string app_name = app_src.filename().string();
  const auto app_dst = std::filesystem::path("/Applications") / app_name;

  // cp -R to /Applications (overwrites existing).
  std::string cp_cmd =
      "cp -R \"" + app_src.string() + "\" \"" + app_dst.string() + "\"";
  rc = std::system(cp_cmd.c_str());

  // Unmount regardless of copy outcome.
  std::system(("hdiutil detach \"" + mount_point + "\"").c_str());

  if (rc != 0) {
    result.error_message = "Failed to copy .app to /Applications (exit " +
                           std::to_string(rc) + ")";
    return result;
  }

  result.success = true;
  result.requires_restart = true;
  return result;
}

void MacOSSelfUpdater::restart() {
  // open -a GameModManager then exit.
  std::system("open -a GameModManager");
  std::exit(0);
}

} // namespace engine::update

#endif // __APPLE__
