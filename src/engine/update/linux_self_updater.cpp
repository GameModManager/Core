#include "engine/update/linux_self_updater.h"

#ifdef __linux__

#include <cstdlib>
#include <filesystem>

#include "engine/core/log/logger.h"
#include "engine/source/download/curl_download.h"
#include "engine/update/self_updater_p.h"

namespace engine::update {

std::unique_ptr<SelfUpdater> create_linux_updater() {
  return std::make_unique<LinuxSelfUpdater>();
}

UpdateInfo LinuxSelfUpdater::check_for_update() {
  // Try AppImage suffix first, then generic binary.
  UpdateInfo info = fetch_update_info(".AppImage");
  if (!info.available)
    info = fetch_update_info(".tar.gz");
  return info;
}

InstallResult LinuxSelfUpdater::install_update(
    const UpdateInfo &info, std::function<void(float progress)> progress_cb) {
  InstallResult result;

  if (!info.available || info.download_url.empty()) {
    result.error_message = "No update available or download URL missing.";
    return result;
  }

  const auto dest = std::filesystem::temp_directory_path() / "gmm_update";

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

  bool ok =
      dl::curl_download(info.download_url, dest, http_code, opts, &dl_progress);
  if (!ok || http_code >= 400) {
    result.error_message =
        "Download failed (HTTP " + std::to_string(http_code) + ")";
    return result;
  }

  if (progress_cb)
    progress_cb(1.0f);

  // Make executable.
  std::filesystem::permissions(dest,
                               std::filesystem::perms::owner_exec |
                                   std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::add);

  result.success = true;
  result.requires_restart = true;
  return result;
}

void LinuxSelfUpdater::restart() {
  const auto exe = std::filesystem::current_path() / "gamemodmanager";
  if (std::filesystem::exists(exe)) {
    exe.native();
    std::string cmd = "\"" + exe.string() + "\" &";
    std::system(cmd.c_str());
  }
  std::exit(0);
}

} // namespace engine::update

#endif // __linux__
