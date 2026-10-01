#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace engine::update {

// ---------------------------------------------------------------------------
// Self-updater strategy pattern - platform-specific update implementations
// ---------------------------------------------------------------------------
// The base class defines the interface; factory::create() returns the concrete
// updater for the current platform and distribution type.

struct UpdateInfo {
  bool available = false;
  std::string version;
  std::string download_url;
  std::string changelog;
  int64_t download_size = 0;
};

struct InstallResult {
  bool success = false;
  std::string error_message;
  bool requires_restart = false;
};

class SelfUpdater {
public:
  virtual ~SelfUpdater() = default;

  // Check if an update is available (queries GitHub API or platform store).
  virtual UpdateInfo check_for_update() = 0;

  // Download and install the update. progress_cb receives [0.0, 1.0].
  virtual InstallResult
  install_update(const UpdateInfo &info,
                 std::function<void(float progress)> progress_cb = {}) = 0;

  // Restart the application (called after a successful install).
  virtual void restart() = 0;

  // Factory: create the right updater for this platform, chosen by
  // detect_install_method(). Returns nullptr when this install has no updater
  // in this build (QtIFW, Flatpak, or a layout we do not recognise) rather
  // than guessing one.
  static std::unique_ptr<SelfUpdater> create();
};

}  // namespace engine::update
