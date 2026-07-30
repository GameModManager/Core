#include "engine/index/conflict_engine.h"

#include "engine/log/logger.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace engine {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::vector<std::string> ConflictEngine::split_csv(const std::string& csv) {
    std::vector<std::string> result;
    std::istringstream ss(csv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        auto s = token.find_first_not_of(" \t");
        auto e = token.find_last_not_of(" \t");
        if (s != std::string::npos)
            result.push_back(token.substr(s, e - s + 1));
    }
    return result;
}

// ---------------------------------------------------------------------------
// Quick token
// ---------------------------------------------------------------------------

std::string ConflictEngine::compute_quick_token(const std::filesystem::path& mod_path) {
    std::error_code ec;
    auto ftime = std::filesystem::last_write_time(mod_path, ec);
    if (ec) return {};

    // Count entries at the top level
    int count = 0;
    for (auto it = std::filesystem::directory_iterator(mod_path, ec);
         it != std::filesystem::directory_iterator{}; ++it) {
        if (!ec) ++count;
    }
    if (ec) return {};

    // Stable token: mtime epoch + entry count
    auto epoch = ftime.time_since_epoch().count();
    return std::to_string(epoch) + "_" + std::to_string(count);
}

// ---------------------------------------------------------------------------
// Walk a single mod directory, collecting conflict-relevant paths
// ---------------------------------------------------------------------------

std::vector<std::string> ConflictEngine::walk_mod(
    const std::filesystem::path& mod_path,
    const std::vector<std::string>& extensions,
    const std::vector<std::string>& ignored)
{
    // Build a set of ignored filenames for O(1) lookup
    std::unordered_set<std::string> ignore_set(ignored.begin(), ignored.end());

    std::vector<std::string> files;
    std::error_code ec;

    if (!std::filesystem::exists(mod_path, ec)) return files;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(mod_path, ec)) {
        if (ec) break;

        if (!entry.is_regular_file(ec)) continue;
        if (ec) break;

        auto filename = entry.path().filename().string();
        if (ignore_set.find(filename) != ignore_set.end()) continue;

        auto rel = std::filesystem::relative(entry.path(), mod_path, ec);
        if (ec) continue;

        auto rel_str = rel.string();

        // Filter by extension (if any extension filters are configured)
        if (!extensions.empty()) {
            auto ext = entry.path().extension().string();
            bool found = false;
            for (const auto& filter_ext : extensions) {
                // Normalise: both should have a leading dot
                std::string fe = filter_ext;
                if (fe.empty()) continue;
                if (fe[0] != '.') fe = "." + fe;
                if (ext == fe) { found = true; break; }
            }
            if (!found) continue;
        }

        files.push_back(rel_str);
    }

    // Stable sort for reproducible cache entries
    std::sort(files.begin(), files.end());
    return files;
}

// ---------------------------------------------------------------------------
// JSON cache I/O  (nlohmann::json)
// ---------------------------------------------------------------------------

std::unordered_map<std::string, ConflictEngine::FileCache>
ConflictEngine::load_cache(const std::filesystem::path& cache_path) const {
    if (cache_path.empty() || !std::filesystem::exists(cache_path))
        return {};

    std::ifstream f(cache_path);
    if (!f) return {};

    try {
        json j;
        f >> j;

        std::unordered_map<std::string, FileCache> cache;
        if (j.contains("mods")) {
            for (const auto& [key, val] : j["mods"].items()) {
                FileCache fc;
                fc.token = val["token"].get<std::string>();
                for (const auto& fp : val["files"])
                    fc.files.push_back(fp.get<std::string>());
                cache[key] = std::move(fc);
            }
        }
        return cache;
    } catch (...) {
        return {};
    }
}

void ConflictEngine::save_cache(
    const std::filesystem::path& cache_path,
    const std::unordered_map<std::string, FileCache>& cache) const
{
    if (cache_path.empty()) return;

    // Ensure parent directory exists
    std::error_code ec;
    std::filesystem::create_directories(cache_path.parent_path(), ec);
    if (ec) return;

    json j;
    j["version"] = 1;

    json mods_obj;
    for (const auto& [folder, fc] : cache) {
        json entry;
        entry["token"] = fc.token;
        entry["files"] = fc.files;
        mods_obj[folder] = std::move(entry);
    }
    j["mods"] = std::move(mods_obj);

    std::ofstream f(cache_path);
    if (f) f << j.dump(2);
}

// ---------------------------------------------------------------------------
// Main compute
// ---------------------------------------------------------------------------

std::unordered_map<std::string, ConflictStats> ConflictEngine::compute(
    const std::filesystem::path& mods_dir,
    const std::vector<ModInfo>& mods,
    const std::string& extensions_csv,
    const std::string& ignored_csv,
    bool conflict_reversed,
    const std::filesystem::path& cache_path)
{
    if (mods_dir.empty() || mods.empty()) return {};

    auto extensions = split_csv(extensions_csv);
    auto ignored    = split_csv(ignored_csv);

    // Load cached file lists
    auto cache = load_cache(cache_path);

    // Phase 1 — collect file lists (walk only changed mods)
    // mod_folder → vector of relative paths
    std::unordered_map<std::string, std::vector<std::string>> file_lists;

    for (const auto& [folder_name, priority] : mods) {
        auto mod_path = mods_dir / folder_name;
        auto token = compute_quick_token(mod_path);

        auto it = cache.find(folder_name);
        if (it != cache.end() && it->second.token == token && !it->second.files.empty()) {
            // Cache hit — use stored files
            file_lists[folder_name] = it->second.files;
        } else {
            // Cache miss — walk the directory
            auto files = walk_mod(mod_path, extensions, ignored);
            file_lists[folder_name] = files;

            // Update cache
            cache[folder_name] = {std::move(token), files};
        }
    }

    // Remove stale cache entries (mods that no longer exist)
    std::unordered_set<std::string> active;
    for (const auto& [fn, _] : mods) active.insert(fn);
    for (auto it = cache.begin(); it != cache.end(); ) {
        if (active.find(it->first) == active.end())
            it = cache.erase(it);
        else
            ++it;
    }

    // Write cache back
    save_cache(cache_path, cache);

    // Phase 2 — build path → owners registry
    // For each file, record (folder_name, priority) for every mod that owns it
    PathRegistry path_registry;
    for (const auto& [folder_name, priority] : mods) {
        auto it = file_lists.find(folder_name);
        if (it == file_lists.end()) continue;
        for (const auto& rel_path : it->second) {
            path_registry[rel_path].emplace_back(folder_name, priority);
        }
    }

    // Phase 3 — derive per-mod conflict stats
    std::unordered_map<std::string, ConflictStats> results;
    for (const auto& [folder_name, priority] : mods) {
        auto it = file_lists.find(folder_name);
        if (it == file_lists.end()) continue;

        ConflictStats stats;
        stats.total_files = static_cast<int>(it->second.size());

        for (const auto& rel_path : it->second) {
            auto& owners = path_registry[rel_path];
            if (owners.size() <= 1) continue;  // no conflict

            // The owner with the "winning" priority is:
            //   reversed=false → highest priority number
            //   reversed=true  → lowest  priority number
            if (conflict_reversed) {
                // Isaac convention: lower number = higher priority = wins
                auto& winner = *std::min_element(owners.begin(), owners.end(),
                    [](const auto& a, const auto& b) { return a.second < b.second; });
                if (winner.first == folder_name)
                    ++stats.wins;
                else
                    ++stats.losses;
            } else {
                // Standard MO2: higher number = higher priority = wins
                auto& winner = *std::max_element(owners.begin(), owners.end(),
                    [](const auto& a, const auto& b) { return a.second < b.second; });
                if (winner.first == folder_name)
                    ++stats.wins;
                else
                    ++stats.losses;
            }
        }

        results[folder_name] = stats;
    }

    registry_ = std::move(path_registry);
    return results;
}

void ConflictEngine::invalidate_mod(const std::string& folder_name,
                                     const std::filesystem::path& cache_path) {
    if (cache_path.empty()) return;

    auto cache = load_cache(cache_path);
    cache.erase(folder_name);
    save_cache(cache_path, cache);
}

}  // namespace engine
