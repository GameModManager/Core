#include "engine/update/appimage_updater.h"

#ifdef __linux__

#include <cstdlib>
#include <filesystem>

#include "engine/core/log/logger.h"
#include "engine/source/download/curl_download.h"
#include "engine/update/self_updater_p.h"

namespace engine::update {

std::unique_ptr<SelfUpdater> create_appimage_updater() {
  return std::make_unique<AppImageUpdater>();
}

UpdateInfo AppImageUpdater::check_for_update() {
  return fetch_update_info(".AppImage");
}

InstallResult AppImageUpdater::install_update(
    const UpdateInfo &info, std::function<void(float progress)> progress_cb) {
  InstallResult result;

  if (!info.available || info.download_url.empty()) {
    result.error_message = "No update available or download URL missing.";
    return result;
  }

  // The running AppImage path is in the APPIMAGE env var.
  const char *appimage_path = std::getenv("APPIMAGE");
  if (!appimage_path || appimage_path[0] == '\0') {
    result.error_message = "APPIMAGE environment variable not set.";
    return result;
  }
  const auto current_path = std::filesystem::path(appimage_path);

  // Download the new AppImage to a temporary location, then rename into
  // place. This avoids truncating the running binary.
  const auto tmp_path =
      std::filesystem::temp_directory_path() / "gmm_update.AppImage";

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

  bool ok = dl::curl_download(info.download_url, tmp_path, http_code, opts,
                              &dl_progress);
  if (!ok || http_code >= 400) {
    result.error_message =
        "Download failed (HTTP " + std::to_string(http_code) + ")";
    return result;
  }

  if (progress_cb)
    progress_cb(1.0f);

  // Make the new AppImage executable.
  std::filesystem::permissions(tmp_path,
                               std::filesystem::perms::owner_exec |
                                   std::filesystem::perms::owner_read,
                               std::filesystem::perm_options::add);

  // Move the new file into place (atomic on the same filesystem).
  std::error_code ec;
  std::filesystem::rename(tmp_path, current_path, ec);
  if (ec) {
    // Cross-device move fallback: copy then remove.
    std::filesystem::copy_file(
        tmp_path, current_path,
        std::filesystem::copy_options::overwrite_existing, ec);
    std::filesystem::remove(tmp_path);
    if (ec) {
      result.error_message = "Failed to replace AppImage: " + ec.message();
      return result;
    }
  }

  result.success = true;
  result.requires_restart = true;
  return result;
}

void AppImageUpdater::restart() {
  const char *appimage_path = std::getenv("APPIMAGE");
  if (appimage_path && appimage_path[0] != '\0') {
    std::string cmd = "\"" + std::string(appimage_path) + "\" &";
    std::system(cmd.c_str());
  }
  std::exit(0);
}

} // namespace engine::update

#endif // __linux__
