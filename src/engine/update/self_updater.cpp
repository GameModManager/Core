#include "engine/update/self_updater.h"
#include "engine/update/self_updater_p.h"

#include <cstdlib>
#include <fstream>
#include <regex>

#include "engine/core/log/logger.h"
#include "engine/source/download/curl_download.h"

namespace engine::update {

namespace {

// GitHub API endpoint for the latest release.
constexpr const char *kGitHubApiUrl =
    "https://api.github.com/repos/GameModManager/GMM/releases/latest";
constexpr const char *kUserAgent = "GameModManager/SelfUpdater";

// Fetch the raw JSON body from the GitHub releases API.
bool fetch_github_latest(nlohmann::json &out) {
  namespace dl = engine::Source::DownloadManager;

  const auto tmp = std::filesystem::temp_directory_path() / "gmm_update.json";
  long http_code = 0;
  dl::Options opts;
  opts.user_agent = kUserAgent;

  bool ok = dl::curl_download(kGitHubApiUrl, tmp, http_code, opts);
  if (!ok || http_code >= 400) {
    Logger::instance().error("SelfUpdater: GitHub API request failed (HTTP " +
                             std::to_string(http_code) + ")");
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    return false;
  }

  std::ifstream ifs(tmp);
  if (!ifs) {
    Logger::instance().error("SelfUpdater: cannot read GitHub response");
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    return false;
  }

  try {
    ifs >> out;
  } catch (const std::exception &e) {
    Logger::instance().error(std::string("SelfUpdater: JSON parse error: ") +
                             e.what());
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    return false;
  }

  std::error_code ec;
  std::filesystem::remove(tmp, ec);
  return true;
}

// Parse a "v0.4.2" tag into a comparable triple.
bool parse_version(const std::string &tag, int &major, int &minor, int &patch) {
  std::string v = tag;
  if (!v.empty() && v[0] == 'v')
    v = v.substr(1);
  std::regex re(R"((\d+)\.(\d+)\.(\d+))");
  std::smatch m;
  if (!std::regex_match(v, m, re))
    return false;
  major = std::stoi(m[1]);
  minor = std::stoi(m[2]);
  patch = std::stoi(m[3]);
  return true;
}

// Find the best asset URL matching the platform suffix.
std::string find_asset_url(const nlohmann::json &release,
                           const std::string &suffix) {
  if (!release.contains("assets") || !release["assets"].is_array())
    return {};
  for (const auto &asset : release["assets"]) {
    if (!asset.contains("name") || !asset.contains("browser_download_url"))
      continue;
    const std::string name = asset["name"].get<std::string>();
    if (name.size() >= suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
      return asset["browser_download_url"].get<std::string>();
    }
  }
  return {};
}

} // namespace

// ---------------------------------------------------------------------------
// Shared GitHub helpers for subclass use
// ---------------------------------------------------------------------------

UpdateInfo fetch_update_info(const std::string &asset_suffix,
                             bool include_prereleases) {
  UpdateInfo info;
  nlohmann::json release;
  if (!fetch_github_latest(release))
    return info;

  if (release.contains("prerelease") && release["prerelease"].get<bool>() &&
      !include_prereleases) {
    return info;
  }

  const std::string tag = release.value("tag_name", "");
  if (tag.empty())
    return info;

  int major = 0, minor = 0, patch = 0;
  if (!parse_version(tag, major, minor, patch))
    return info;

  std::regex ver_re(R"((\d+)\.(\d+)\.(\d+))");
  std::smatch cur_m;
  std::string cur_ver(VERSION);
  if (std::regex_search(cur_ver, cur_m, ver_re)) {
    int c_major = std::stoi(cur_m[1]);
    int c_minor = std::stoi(cur_m[2]);
    int c_patch = std::stoi(cur_m[3]);
    if (std::make_tuple(major, minor, patch) <=
        std::make_tuple(c_major, c_minor, c_patch)) {
      return info;
    }
  }

  info.available = true;
  info.version = tag;
  info.changelog = release.value("body", "");
  info.download_url = find_asset_url(release, asset_suffix);

  return info;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<SelfUpdater> SelfUpdater::create() {
#if defined(_WIN32)
  extern std::unique_ptr<SelfUpdater> create_windows_updater();
  return create_windows_updater();
#elif defined(__APPLE)
  extern std::unique_ptr<SelfUpdater> create_macos_updater();
  return create_macos_updater();
#elif defined(__linux__)
  const std::string distro = detect_distro_type();
  Logger::instance().info("SelfUpdater: detected distro type = " + distro);

  if (distro == "flatpak") {
    extern std::unique_ptr<SelfUpdater> create_flatpak_updater();
    return create_flatpak_updater();
  }
  if (distro == "appimage") {
    extern std::unique_ptr<SelfUpdater> create_appimage_updater();
    return create_appimage_updater();
  }
  if (distro == "deb" || distro == "rpm") {
    extern std::unique_ptr<SelfUpdater> create_debrpm_updater(
        const std::string &pkg_type);
    return create_debrpm_updater(distro);
  }
  if (distro == "aur") {
    extern std::unique_ptr<SelfUpdater> create_aur_updater();
    return create_aur_updater();
  }

  extern std::unique_ptr<SelfUpdater> create_linux_updater();
  return create_linux_updater();
#else
  return nullptr;
#endif
}

// ---------------------------------------------------------------------------
// Distro detection (Linux only)
// ---------------------------------------------------------------------------

std::string SelfUpdater::detect_distro_type() {
#if !defined(__linux__)
  return "unknown";
#else
  if (const char *flatpak_id = std::getenv("FLATPAK_ID")) {
    if (flatpak_id[0] != '\0')
      return "flatpak";
  }

  if (const char *appimage = std::getenv("APPIMAGE")) {
    if (appimage[0] != '\0')
      return "appimage";
  }

  {
    FILE *pipe = popen("pacman -Q gamemodmanager 2>/dev/null", "r");
    if (pipe) {
      char buf[64];
      if (std::fgets(buf, sizeof(buf), pipe)) {
        pclose(pipe);
        return "aur";
      }
      pclose(pipe);
    }
  }

  {
    FILE *pipe = popen("dpkg -l gamemodmanager 2>/dev/null", "r");
    if (pipe) {
      char buf[64];
      if (std::fgets(buf, sizeof(buf), pipe)) {
        pclose(pipe);
        return "deb";
      }
      pclose(pipe);
    }
  }
  {
    FILE *pipe = popen("rpm -q gamemodmanager 2>/dev/null", "r");
    if (pipe) {
      char buf[64];
      if (std::fgets(buf, sizeof(buf), pipe)) {
        pclose(pipe);
        return "rpm";
      }
      pclose(pipe);
    }
  }

  return "unknown";
#endif
}

} // namespace engine::update
