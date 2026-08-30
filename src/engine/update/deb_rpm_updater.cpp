#include "engine/update/deb_rpm_updater.h"

#ifdef __linux__

#include <cstdlib>
#include <filesystem>

#include "engine/core/log/logger.h"
#include "engine/source/download/curl_download.h"
#include "engine/update/self_updater_p.h"

namespace engine::update {

std::unique_ptr<SelfUpdater>
create_debrpm_updater(const std::string &pkg_type) {
  return std::make_unique<DebRpmUpdater>(pkg_type);
}

DebRpmUpdater::DebRpmUpdater(std::string pkg_type)
    : pkg_type_(std::move(pkg_type)) {}

UpdateInfo DebRpmUpdater::check_for_update() {
  if (pkg_type_ == "deb")
    return fetch_update_info(".deb");
  if (pkg_type_ == "rpm")
    return fetch_update_info(".rpm");
  return {};
}

InstallResult
DebRpmUpdater::install_update(const UpdateInfo &info,
                              std::function<void(float progress)> progress_cb) {
  InstallResult result;

  if (!info.available || info.download_url.empty()) {
    result.error_message = "No update available or download URL missing.";
    return result;
  }

  const std::string ext = (pkg_type_ == "deb") ? ".deb" : ".rpm";
  const auto pkg_path =
      std::filesystem::temp_directory_path() / ("gmm_update" + ext);

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

  bool ok = dl::curl_download(info.download_url, pkg_path, http_code, opts,
                              &dl_progress);
  if (!ok || http_code >= 400) {
    result.error_message =
        "Download failed (HTTP " + std::to_string(http_code) + ")";
    return result;
  }

  if (progress_cb)
    progress_cb(1.0f);

  // Install via the system package manager with elevated privileges.
  std::string cmd;
  if (pkg_type_ == "deb") {
    cmd = "pkexec apt install -y \"" + pkg_path.string() + "\"";
  } else {
    cmd = "pkexec rpm -Uvh \"" + pkg_path.string() + "\"";
  }

  int rc = std::system(cmd.c_str());
  if (rc != 0) {
    result.error_message =
        "Package install failed (exit " + std::to_string(rc) + ")";
    return result;
  }

  result.success = true;
  result.requires_restart = true;
  return result;
}

void DebRpmUpdater::restart() {
  const auto exe = std::filesystem::current_path() / "gamemodmanager";
  if (std::filesystem::exists(exe)) {
    std::string cmd = "\"" + exe.string() + "\" &";
    std::system(cmd.c_str());
  }
  std::exit(0);
}

} // namespace engine::update

#endif // __linux__
