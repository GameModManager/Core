#include "engine/instance/game_icons.h"

#include "engine/instance/instance_utils.h"
#include "engine/download/curl_download.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace engine {

fs::path icon_cache_dir() {
    return default_instances_dir().parent_path() / "cache" / "icons";
}

fs::path cached_icon_path(const std::string& game_id) {
    return icon_cache_dir() / (game_id + ".png");
}

std::string icon_url_for(const GameKnowledge& knowledge,
                         const std::string& game_id) {
    return knowledge.get(game_id, kIconUrlKey, "");
}

bool ensure_icon_cached(const std::string& game_id,
                        const std::string& url,
                        std::string& error) {
    error.clear();

    auto path = cached_icon_path(game_id);

    // Already cached (non-empty)?
    std::error_code ec;
    bool cached_ok = false;
    if (fs::is_regular_file(path, ec) && !ec) {
        std::uintmax_t sz = fs::file_size(path, ec);
        cached_ok = !ec && sz > 0;
    }
    if (cached_ok) return true;

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

}  // namespace engine
