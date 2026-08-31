#include "engine/core/github.h"

#include "engine/core/log/logger.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace engine {

namespace {

// ---------------------------------------------------------------------------
// curl write callback - appends received data to a std::string
// ---------------------------------------------------------------------------
size_t write_to_string(void *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *out = static_cast<std::string *>(userdata);
  const size_t bytes = size * nmemb;
  out->append(static_cast<const char *>(ptr), bytes);
  return bytes;
}

// ---------------------------------------------------------------------------
// curl write callback - writes to an output file stream
// ---------------------------------------------------------------------------
size_t write_to_file(void *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *file = static_cast<std::ofstream *>(userdata);
  if (!file || !file->is_open())
    return 0;
  const auto written = static_cast<std::streamsize>(size * nmemb);
  file->write(static_cast<const char *>(ptr), written);
  return file->good() ? (size * nmemb) : 0;
}

// ---------------------------------------------------------------------------
// curl xferinfo callback - progress reporting for downloads
// ---------------------------------------------------------------------------
struct DownloadProgress {
  std::function<void(float)> callback;
};

int download_xferinfo(void *user_data, curl_off_t dltotal, curl_off_t dlnow,
                      curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
  auto *dp = static_cast<DownloadProgress *>(user_data);
  if (dp && dp->callback && dltotal > 0) {
    dp->callback(static_cast<float>(dlnow) / static_cast<float>(dltotal));
  }
  return 0;
}

// ---------------------------------------------------------------------------
// HTTP GET - fetch a URL into a string. Returns true on success.
// ---------------------------------------------------------------------------
bool http_get(const std::string &url, std::string &response_body,
              long &http_code) {
  auto *curl = curl_easy_init();
  if (!curl)
    return false;

  std::string body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "GameModManager/0.1");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

  CURLcode res = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK || http_code >= 400) {
    Logger::instance().error("GitHub API GET failed: " + url +
                             " (curl=" + std::string(curl_easy_strerror(res)) +
                             ", http=" + std::to_string(http_code) + ")");
    return false;
  }

  response_body = std::move(body);
  return true;
}

// ---------------------------------------------------------------------------
// Parse a semantic version string into components.
// Handles "v1.2.3", "1.2.3", "1.2.3-beta.1", etc.
// ---------------------------------------------------------------------------
struct SemVer {
  int major = 0;
  int minor = 0;
  int patch = 0;
  std::string prerelease;
};

SemVer parse_version(const std::string &raw) {
  SemVer v;
  std::string s = raw;
  // Strip leading 'v' or 'V'
  if (!s.empty() && (s[0] == 'v' || s[0] == 'V'))
    s = s.substr(1);

  // Split at first non-numeric, non-dot character to isolate prerelease
  auto prerelease_pos = s.find_first_not_of("0123456789.");
  std::string numeric = (prerelease_pos != std::string::npos)
                            ? s.substr(0, prerelease_pos)
                            : s;
  if (prerelease_pos != std::string::npos)
    v.prerelease = s.substr(prerelease_pos);

  // Parse major.minor.patch
  std::istringstream ss(numeric);
  std::string token;
  std::vector<int> parts;
  while (std::getline(ss, token, '.')) {
    try {
      parts.push_back(std::stoi(token));
    } catch (...) {
      parts.push_back(0);
    }
  }
  if (parts.size() >= 1)
    v.major = parts[0];
  if (parts.size() >= 2)
    v.minor = parts[1];
  if (parts.size() >= 3)
    v.patch = parts[2];

  return v;
}

// Compare prerelease strings per SemVer spec: numeric identifiers compared as
// integers, alphanumeric compared lexically. A shorter prerelease list is
// lower when the common prefix is equal.
int compare_prerelease(const std::string &a, const std::string &b) {
  if (a.empty() && b.empty())
    return 0;
  if (a.empty())
    return 1; // no prerelease > has prerelease
  if (b.empty())
    return -1;

  auto split_ids = [](const std::string &s) {
    std::vector<std::string> ids;
    std::istringstream ss(s);
    std::string token;
    while (std::getline(ss, token, '.')) {
      // Strip leading non-alphanumeric (e.g. '-')
      while (!token.empty() && !std::isalnum(static_cast<unsigned char>(token[0])))
        token = token.substr(1);
      ids.push_back(token);
    }
    return ids;
  };

  auto ids_a = split_ids(a);
  auto ids_b = split_ids(b);
  size_t len = std::min(ids_a.size(), ids_b.size());

  for (size_t i = 0; i < len; ++i) {
    bool a_numeric =
        std::all_of(ids_a[i].begin(), ids_a[i].end(), [](char c) {
          return std::isdigit(static_cast<unsigned char>(c));
        });
    bool b_numeric =
        std::all_of(ids_b[i].begin(), ids_b[i].end(), [](char c) {
          return std::isdigit(static_cast<unsigned char>(c));
        });

    if (a_numeric && b_numeric) {
      int ai = std::stoi(ids_a[i]);
      int bi = std::stoi(ids_b[i]);
      if (ai != bi)
        return ai < bi ? -1 : 1;
    } else {
      if (ids_a[i] != ids_b[i])
        return ids_a[i] < ids_b[i] ? -1 : 1;
    }
  }

  return ids_a.size() < ids_b.size() ? -1
      : ids_a.size() > ids_b.size() ? 1
                                    : 0;
}

} // namespace

// ---------------------------------------------------------------------------
// GitHub::latest_release
// ---------------------------------------------------------------------------
std::optional<GitHub::Release>
GitHub::latest_release(const std::string &owner, const std::string &repo,
                       bool include_prereleases) {
  std::string url =
      "https://api.github.com/repos/" + owner + "/" + repo + "/releases";
  if (!include_prereleases)
    url += "/latest";

  std::string body;
  long http_code = 0;
  if (!http_get(url, body, http_code))
    return std::nullopt;

  try {
    auto json = nlohmann::json::parse(body);

    if (include_prereleases) {
      // The /releases endpoint returns an array; find the first entry
      // (already sorted newest-first by GitHub).
      if (!json.is_array() || json.empty())
        return std::nullopt;
      json = json[0];
    }

    Release release;
    release.tag_name = json.value("tag_name", "");
    release.name = json.value("name", "");
    release.body = json.value("body", "");
    release.prerelease = json.value("prerelease", false);

    if (json.contains("assets") && json["assets"].is_array()) {
      for (const auto &a : json["assets"]) {
        Asset asset;
        asset.name = a.value("name", "");
        asset.download_url = a.value("browser_download_url", "");
        asset.size = a.value("size", int64_t(0));
        release.assets.push_back(std::move(asset));
      }
    }

    return release;
  } catch (const std::exception &e) {
    Logger::instance().error("Failed to parse GitHub release JSON: " +
                             std::string(e.what()));
    return std::nullopt;
  }
}

// ---------------------------------------------------------------------------
// GitHub::compare_versions
// ---------------------------------------------------------------------------
int GitHub::compare_versions(const std::string &a, const std::string &b) {
  SemVer va = parse_version(a);
  SemVer vb = parse_version(b);

  if (va.major != vb.major)
    return va.major < vb.major ? -1 : 1;
  if (va.minor != vb.minor)
    return va.minor < vb.minor ? -1 : 1;
  if (va.patch != vb.patch)
    return va.patch < vb.patch ? -1 : 1;

  return compare_prerelease(va.prerelease, vb.prerelease);
}

// ---------------------------------------------------------------------------
// GitHub::download
// ---------------------------------------------------------------------------
bool GitHub::download(const std::string &url,
                      const std::filesystem::path &dest,
                      std::function<void(float)> progress_cb) {
  auto *curl = curl_easy_init();
  if (!curl)
    return false;

  std::ofstream file(dest, std::ios::binary);
  if (!file) {
    curl_easy_cleanup(curl);
    return false;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "GameModManager/0.1");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);

  DownloadProgress dp;
  dp.callback = std::move(progress_cb);
  if (dp.callback) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, download_xferinfo);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &dp);
  }

  CURLcode res = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);
  file.close();

  if (res != CURLE_OK || http_code >= 400) {
    Logger::instance().error("GitHub download failed: " + url +
                             " (curl=" + std::string(curl_easy_strerror(res)) +
                             ", http=" + std::to_string(http_code) + ")");
    std::error_code ec;
    std::filesystem::remove(dest, ec);
    return false;
  }

  return true;
}

} // namespace engine
