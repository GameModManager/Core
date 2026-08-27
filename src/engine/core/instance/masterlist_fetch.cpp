#include "engine/core/instance/masterlist_fetch.h"

#include "engine/core/instance/instance_utils.h"
#include "engine/core/log/logger.h"
#include "engine/source/download/curl_download.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace engine {

fs::path masterlist_cache_dir() {
  return default_instances_dir().parent_path() / "masterlists";
}

fs::path cached_masterlist_path(const std::string &game_id) {
  return masterlist_cache_dir() / game_id / kMasterlistFilename;
}

std::string masterlist_url_for(const GameKnowledge &knowledge,
                               const std::string &game_id) {
  return knowledge.get(game_id, kMasterlistUrlKey, "");
}

bool ensure_masterlist_cached(const std::string &game_id,
                              const std::string &url, std::string &error) {
  error.clear();

  if (url.empty()) {
    error = "no masterlist URL declared";
    return false;
  }

  auto path = cached_masterlist_path(game_id);

  // Already cached (non-empty)?
  std::error_code ec;
  bool cached_ok = false;
  if (fs::is_regular_file(path, ec) && !ec) {
    std::uintmax_t sz = fs::file_size(path, ec);
    cached_ok = !ec && sz > 0;
  }
  if (cached_ok)
    return true;

  // A zero-byte leftover from a previously aborted transfer would be treated
  // as "cached" by the size check — clear it so the download starts fresh.
  fs::remove(path, ec);

  std::error_code mk;
  fs::create_directories(path.parent_path(), mk);

  long http_code = 0;
  if (!engine::download::curl_download(url, path, http_code)) {
    error = "download failed (HTTP " + std::to_string(http_code) + ")";
    return false;
  }
  if (!fs::is_regular_file(path, ec) || ec || fs::file_size(path, ec) == 0) {
    error = "download produced no data";
    return false;
  }
  return true;
}

std::string read_cached_masterlist(const std::string &game_id) {
  auto path = cached_masterlist_path(game_id);

  std::error_code ec;
  if (!fs::is_regular_file(path, ec) || ec) {
    return {};
  }

  std::ifstream ifs(path);
  if (!ifs.is_open()) {
    return {};
  }

  return std::string((std::istreambuf_iterator<char>(ifs)),
                     std::istreambuf_iterator<char>());
}

} // namespace engine
