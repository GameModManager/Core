#include "engine/update/self_updater.h"
#include "engine/update/self_updater_p.h"
#include "engine/update/install_method.h"

#include <cstdlib>
#include <string>

#include "engine/core/github.h"
#include "engine/log/logger.h"
#include "engine/network/network_manager.h"

namespace engine::update {

namespace {

  // Release feed. 0 GitHub releases exist on this repo, so every query below
  // is currently a 404 - the feed is a prerequisite, not a tuning knob.
  constexpr const char *kFeedOwner = "GameModManager";
  constexpr const char *kFeedRepo  = "GMM";

}  // namespace

// ---------------------------------------------------------------------------
// Shared GitHub helpers for subclass use
// ---------------------------------------------------------------------------

std::string find_asset_url(const engine::GitHub::Release &release,
                           const std::string &suffix) {
  // A zero-length suffix is a suffix of every name, so matching it would
  // return the first asset in the list purely by list order. Refuse it
  // instead: an update with no asset for this platform has no URL, and
  // silently picking an arbitrary one is how a .zip gets handed to an
  // installer.
  if (suffix.empty())
    return {};
  for (const auto &asset : release.assets) {
    if (asset.name.size() >= suffix.size() &&
        asset.name.compare(asset.name.size() - suffix.size(), suffix.size(), suffix) ==
            0)
      return asset.download_url;
  }
  return {};
}

UpdateInfo fetch_update_info(const std::string &asset_suffix,
                             bool include_prereleases) {
  UpdateInfo info;

  auto release =
      engine::GitHub::latest_release(kFeedOwner, kFeedRepo, include_prereleases);
  if (!release)
    return info;

  const std::string tag = release->tag_name;
  if (tag.empty())
    return info;

  // GitHub::compare_versions understands a prerelease suffix (v0.6.0-rc1
  // sorts below v0.6.0); the previous full-string regex_match could not
  // parse one at all, so a prerelease tag was treated as unparseable and
  // silently reported as "no update".
  if (engine::GitHub::compare_versions(tag, std::string(VERSION)) <= 0)
    return info;

  info.available    = true;
  info.version      = tag;
  info.changelog    = release->body;
  info.download_url = find_asset_url(*release, asset_suffix);
  if (info.download_url.empty()) {
    // The release exists and is newer, but this platform has no asset for it.
    // Reporting it as available with an empty URL would hand an install path
    // nothing to fetch.
    return UpdateInfo{};
  }

  return info;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<SelfUpdater> SelfUpdater::create() {
  // Detection is read-only and returns a method, not a guess: a layout we do
  // not recognise yields nullptr rather than an updater that would write to
  // the wrong place.
  const InstallMethod method = detect_install_method(probe_install_facts());
  Logger::instance().info(std::string("SelfUpdater: install method = ") +
                          install_method_name(method));

  switch (method) {
#if defined(_WIN32)
  case InstallMethod::WindowsInstaller:
  case InstallMethod::WindowsPortable: {
    extern std::unique_ptr<SelfUpdater> create_windows_updater();
    return create_windows_updater();
  }
#elif defined(__APPLE__)
  case InstallMethod::MacOsDmg:
  case InstallMethod::MacOsMountedImage: {
    extern std::unique_ptr<SelfUpdater> create_macos_updater();
    return create_macos_updater();
  }
#elif defined(__linux__)
  case InstallMethod::AppImage: {
    extern std::unique_ptr<SelfUpdater> create_appimage_updater();
    return create_appimage_updater();
  }
#endif
  // Qt Installer Framework, Flatpak and every unrecognised layout have no
  // updater in this build. They are detected and reported, not written to.
  default:
    return nullptr;
  }
}

}  // namespace engine::update
