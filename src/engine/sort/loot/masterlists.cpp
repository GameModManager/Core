#include "engine/sort/loot/masterlists.h"

#include "engine/source/download/curl_download.h"
#include "engine/core/log/logger.h"
#include "platform/platform.h"

#include <chrono>
#include <optional>
#include <system_error>

namespace fs = std::filesystem;

namespace engine {

namespace {

constexpr const char* kMasterlistFilename = "masterlist.yaml";
constexpr const char* kPreludeFilename = "prelude.yaml";
constexpr const char* kPreludeRepo = "prelude";

std::string raw_url(const std::string& repo, const std::string& branch,
                    const std::string& filename) {
    return "https://raw.githubusercontent.com/loot/" + repo + "/" + branch + "/" +
           filename;
}

// Download url into dest (via a temp file, so a failed refresh never clobbers
// a cached copy). Returns true when a complete file now sits at dest.
bool fetch_file(const std::string& url, const fs::path& dest) {
    fs::path tmp = dest;
    tmp += ".tmp";
    std::error_code ec;
    fs::remove(tmp, ec);
    long http_code = 0;
    const bool ok = download::curl_download(url, tmp, http_code);
    if (!ok) {
        fs::remove(tmp, ec);  // curl_download already does; belt and braces
        return false;
    }
    std::error_code move_ec;
    fs::rename(tmp, dest, move_ec);
    if (move_ec) {
        fs::remove(tmp, move_ec);
        return false;
    }
    return true;
}

bool fresh(const fs::path& p) {
    std::error_code ec;
    const auto mtime = fs::last_write_time(p, ec);
    if (ec) return false;
    return fs::file_time_type::clock::now() - mtime <
           MasterlistManager::kRefreshAfter;
}

// Download `filename` from loot/<repo> probing branch_candidates() in order.
// Returns the branch that succeeded, or nullopt when none did.
std::optional<std::string> download_from_branches(const std::string& repo,
                                                  const std::string& filename,
                                                  const fs::path& dest) {
    for (const auto& branch : MasterlistManager::branch_candidates()) {
        if (fetch_file(raw_url(repo, branch, filename), dest)) return branch;
    }
    return std::nullopt;
}

}  // namespace

MasterlistManager::MasterlistManager(const Platform* platform)
    : platform_(platform) {}

const std::vector<std::string>& MasterlistManager::branch_candidates() {
    static const std::vector<std::string> kCandidates = [] {
        std::vector<std::string> branches;
        // Walk down from the libloot minor this codebase is built against
        // (0.29.x) to the oldest branch any supported game still uses (v0.21),
        // newest first; "master" is the legacy fallback (MO2's
        // oldDefaultBranches sentinel).
        for (int minor = 29; minor >= 21; --minor) {
            branches.push_back("v0." + std::to_string(minor));
        }
        branches.push_back("master");
        return branches;
    }();
    return kCandidates;
}

fs::path MasterlistManager::dir_for(const std::string& game_id) const {
    if (!platform_) return {};
    return platform_->data_dir() / "loot" / game_id;
}

MasterlistManager::Masterlists MasterlistManager::ensure(
    const std::string& game_id, const std::string& repo, std::string* error) {
    return fetch_or_cache(game_id, repo, /*force=*/false, error);
}

MasterlistManager::Masterlists MasterlistManager::update(
    const std::string& game_id, const std::string& repo, std::string* error) {
    return fetch_or_cache(game_id, repo, /*force=*/true, error);
}

MasterlistManager::Masterlists MasterlistManager::fetch_or_cache(
    const std::string& game_id, const std::string& repo, bool force,
    std::string* error) {
    Masterlists out;
    const fs::path dir = dir_for(game_id);
    if (dir.empty()) {
        if (error) *error = "no platform data dir available";
        return out;
    }
    const fs::path masterlist_path = dir / kMasterlistFilename;
    const fs::path prelude_path = dir / kPreludeFilename;

    const bool have_masterlist = fs::is_regular_file(masterlist_path);
    const bool have_prelude = fs::is_regular_file(prelude_path);
    const bool fresh_masterlist = have_masterlist && fresh(masterlist_path);
    const bool fresh_prelude = have_prelude && fresh(prelude_path);

    if (!force && fresh_masterlist && fresh_prelude) {
        out.masterlist = masterlist_path;
        out.prelude = prelude_path;
        out.cached = true;
        return out;
    }

    std::error_code ec;
    fs::create_directories(dir, ec);

    // Masterlist: refresh when stale/missing (probing branch_candidates in
    // order), else keep the cached copy. A failed refresh keeps the cache.
    std::optional<std::string> branch;
    if (force || !fresh_masterlist) {
        branch = download_from_branches(repo, kMasterlistFilename,
                                        masterlist_path);
    }
    if (branch || have_masterlist) {
        out.masterlist = masterlist_path;
        if (!branch) out.cached = true;
    }

    // Prelude: the masterlist's branch first, then a walk-down of the prelude
    // repo itself, then the cache. Refreshed only when stale/missing.
    bool prelude_ok = false;
    if (force || !fresh_prelude) {
        if (branch &&
            fetch_file(raw_url(kPreludeRepo, *branch, kPreludeFilename),
                       prelude_path)) {
            prelude_ok = true;
        }
        if (!prelude_ok &&
            download_from_branches(kPreludeRepo, kPreludeFilename,
                                   prelude_path)) {
            prelude_ok = true;
        }
    }
    if (prelude_ok) {
        out.prelude = prelude_path;
    } else if (have_prelude) {
        out.prelude = prelude_path;  // offline: keep the stale copy
        out.cached = true;
        prelude_ok = true;
    }

    if (out.masterlist.empty() || !prelude_ok) {
        if (error) {
            *error = out.masterlist.empty()
                         ? "masterlist unavailable (offline and no cached copy)"
                         : "prelude unavailable (offline and no cached copy)";
        }
        Logger::instance().warn(
            "Masterlist manager: " +
            std::string(out.masterlist.empty() ? "masterlist" : "prelude") +
            " unavailable for " + game_id + " (repo " + repo + ")");
        return Masterlists{};
    }
    return out;
}

}  // namespace engine
