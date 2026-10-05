#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// GitHub API client - fetches releases, compares versions, downloads assets.
// ---------------------------------------------------------------------------
class GitHub {
public:
  struct Asset {
    std::string name;
    std::string download_url;
    int64_t size = 0;
  };

  struct Release {
    std::string tag_name;
    std::string name;
    std::string body;  // changelog
    std::vector<Asset> assets;
    bool prerelease = false;
  };

  // The releases endpoint a release query uses. include_prereleases picks
  // /releases (the full list, drafts and prereleases included, newest first)
  // over /releases/latest, which by GitHub's definition returns the most
  // recent NON-prerelease, non-draft release and can therefore never yield a
  // prerelease. Exposed because that choice is the whole meaning of the flag:
  // a parameter that cannot change the URL cannot change the answer.
  [[nodiscard]] static std::string releases_endpoint(const std::string &owner,
                                                     const std::string &repo,
                                                     bool include_prereleases);

  // Fetch the latest release from a GitHub repository.
  // When include_prereleases is false, only stable releases are returned.
  // Returns std::nullopt on network error or when no release exists.
  static std::optional<Release> latest_release(const std::string &owner,
                                               const std::string &repo,
                                               bool include_prereleases = false);

  // Compare two semantic version strings (e.g. "1.2.3" vs "1.2.4").
  // Returns -1 if a < b, 0 if a == b, 1 if a > b.
  static int compare_versions(const std::string &a, const std::string &b);

  // Download a file from url to dest on disk.
  // progress_cb receives [0.0, 1.0] progress updates.
  // Returns true on success.
  static bool download(const std::string &url, const std::filesystem::path &dest,
                       std::function<void(float)> progress_cb = {});
};

}  // namespace engine
