#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

using PathRegistry = std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>;

struct ConflictStats {
    int wins = 0;
    int losses = 0;
    int total_files = 0;
};

// Unified conflict computation engine.
//
// Quick-token caching: each mod dir's mtime + file-count is stored alongside
// its file list.  On subsequent runs, only mods whose token changed get
// re-scanned.  The cache is persisted as JSON at the caller's chosen path.
class ConflictEngine {
public:
    ConflictEngine() = default;

    ConflictEngine(const ConflictEngine&) = delete;
    ConflictEngine& operator=(const ConflictEngine&) = delete;

    using ModInfo = std::pair<std::string, int>;

    // Compute per-mod conflict stats for all mods.
    //   mods_dir        — absolute path to the directory containing mod folders
    //   extra_mods_dir  — additional dir to search for mod folders (e.g. game's native mods dir).
    //                     For each mod, mods_dir is tried first, then extra_mods_dir.
    //   mods            — list of (folder_name, priority) for every mod (excl. Overwrite / separators)
    //   extensions_csv  — comma-separated list of extensions to track (e.g. ".png,.lua").
    //                     If empty, all files are tracked.
    //   ignored_csv     — comma-separated list of filenames to skip (e.g. "metadata.xml,disable.it")
    //   conflict_reversed — if true, lower priority number = wins (Isaac convention)
    //   cache_path      — path to the JSON cache file (empty = no caching)
    //   scan_dirs_csv   — comma-separated list of subdirectory names to restrict scanning to
    //                     (e.g. "resources,resources-dlc3" for Isaac). Empty = scan everything.
    [[nodiscard]] std::unordered_map<std::string, ConflictStats> compute(
        const std::filesystem::path& mods_dir,
        const std::vector<ModInfo>& mods,
        const std::string& extensions_csv,
        const std::string& ignored_csv,
        bool conflict_reversed,
        const std::filesystem::path& cache_path = {},
        const std::filesystem::path& extra_mods_dir = {},
        const std::string& scan_dirs_csv = {});

    // Access the file-level registry from the last compute() call.
    // Maps each file path → list of (mod_name, priority) pairs that provide it.
    [[nodiscard]] const PathRegistry& last_registry() const { return registry_; }

    void invalidate_mod(const std::string& folder_name,
                        const std::filesystem::path& cache_path);

private:
    struct FileCache {
        std::string token;
        std::vector<std::string> files;
    };

    static std::string compute_quick_token(const std::filesystem::path& mod_path);

    static std::vector<std::string> walk_mod(
        const std::filesystem::path& mod_path,
        const std::vector<std::string>& extensions,
        const std::vector<std::string>& ignored,
        const std::vector<std::string>& scan_dirs = {});

    struct CacheData {
        std::unordered_map<std::string, FileCache> mods;
        size_t filters_hash = 0;
    };

    // Load/save cache from JSON file
    CacheData load_cache(const std::filesystem::path& cache_path) const;
    void save_cache(const std::filesystem::path& cache_path,
                    const CacheData& data) const;

    // Split CSV string into trimmed tokens
    static std::vector<std::string> split_csv(const std::string& csv);

    PathRegistry registry_;
};

}  // namespace engine
