#include "engine/update/windows_self_updater.h"

#ifdef _WIN32

#include <Windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "engine/core/log/logger.h"
#include "engine/source/download/curl_download.h"

namespace engine::update {

std::unique_ptr<SelfUpdater> create_windows_updater() {
  return std::make_unique<WindowsSelfUpdater>();
}

UpdateInfo WindowsSelfUpdater::check_for_update() {
  return fetch_update_info(".exe");
}

InstallResult WindowsSelfUpdater::install_update(
    const UpdateInfo &info, std::function<void(float progress)> progress_cb) {
  InstallResult result;

  if (!info.available || info.download_url.empty()) {
    result.error_message = "No update available or download URL missing.";
    return result;
  }

  // Download installer to a temp path.
  const auto installer_path =
      std::filesystem::temp_directory_path() / "gmm_setup.exe";

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

  bool ok = dl::curl_download(info.download_url, installer_path, http_code,
                              opts, &dl_progress);
  if (!ok || http_code >= 400) {
    result.error_message =
        "Download failed (HTTP " + std::to_string(http_code) + ")";
    return result;
  }

  if (progress_cb)
    progress_cb(1.0f);

  // Launch the installer silently.
  // NSIS supports /S, Inno Setup supports /SILENT /VERYSILENT.
  std::string cmd = "\"" + installer_path.string() + "\" /S";
  int rc = std::system(cmd.c_str());
  if (rc != 0) {
    result.error_message = "Installer exited with code " + std::to_string(rc);
    std::error_code ec;
    std::filesystem::remove(installer_path, ec);
    return result;
  }

  result.success = true;
  result.requires_restart = true;
  return result;
}

void WindowsSelfUpdater::restart() {
  // The installer is expected to have placed the new binary. Exit and let
  // the user re-launch, or attempt to re-execute the current binary path.
  const auto exe = std::filesystem::current_path() / "gamemodmanager.exe";
  if (std::filesystem::exists(exe)) {
    ShellExecuteA(nullptr, "open", exe.string().c_str(), nullptr, nullptr,
                  SW_SHOW);
  }
  std::exit(0);
}

} // namespace engine::update

#endif // _WIN32
