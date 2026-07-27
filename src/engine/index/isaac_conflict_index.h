#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine {

// Isaac-specific conflict index with fingerprint-based cache invalidation.
//
// Walks mod folders looking for conflict-relevant files (.png, .anm2, .wav, .lua),
// fingerprints each folder using blake2b over file paths + mtimes, and caches
// results in SQLite so unchanged mods skip the full walk on relaunch.
//
// The conflict index answers: "for a given relative path, which mods own it,
// and which one wins (highest priority = lowest list position)?"
class IsaacConflictIndex {
public:
    // relative_path -> list of (mod_folder, priority) sorted by priority ascending
    using ConflictMap = std::unordered_map<std::string,
        std::vector<std::pair<std::string, int>>>;

    // Set the base mods directory and the set of conflict-relevant extensions
    void set_mods_path(const std::string& mods_path);
    void set_conflict_extensions(const std::unordered_set<std::string>& extensions);
    void set_ignored_files(const std::unordered_set<std::string>& ignored);

    // Scan all mod folders and build the conflict index.
    // SQLite database path is used for fingerprint caching.
    void scan(const std::string& db_path);

    // Re-scan a single mod folder (e.g. after rename or file change)
    void rescan_mod(const std::string& mod_folder, const std::string& db_path);

    // Remove a mod from the index (e.g. after delete)
    void remove_mod(const std::string& mod_folder);

    // Query: all files that conflict (appear in 2+ mods)
    [[nodiscard]] const ConflictMap& conflicts() const { return conflicts_; }

    // Query: which mods own a specific relative path
    [[nodiscard]] std::vector<std::pair<std::string, int>>
    owners_of(const std::string& relative_path) const;

    // Query: does this relative path have conflicts?
    [[nodiscard]] bool has_conflict(const std::string& relative_path) const;

    // Query: the winning mod for a relative path (lowest priority number = top of list)
    [[nodiscard]] std::string winner_of(const std::string& relative_path) const;

    // Get all conflict-relevant files for a specific mod
    [[nodiscard]] std::unordered_set<std::string>
    files_for_mod(const std::string& mod_folder) const;

    // Total number of conflicting paths
    [[nodiscard]] size_t conflict_count() const { return conflicts_.size(); }

    // Clear everything
    void clear();

    // Fingerprint cache entry (public for test access)
    struct CachedFingerprint {
        std::string fingerprint;
        std::string files_json;
        std::string token;
    };

private:
    // Walk a single mod folder, return set of relative conflict-relevant paths
    std::unordered_set<std::string> walk_mod(const std::string& mod_path) const;

    // Compute blake2b fingerprint for a mod folder
    std::string fingerprint_folder(const std::string& mod_path) const;

    // Quick token: directory mtime + top-level entry list (fast change detection)
    std::string quick_token(const std::string& mod_path) const;

    // Load/save fingerprints from SQLite cache
    std::optional<CachedFingerprint> load_cache(
        const std::string& db_path, const std::string& mod_folder) const;
    void save_cache(const std::string& db_path, const std::string& mod_folder,
                    const std::string& fingerprint,
                    const std::vector<std::string>& files,
                    const std::string& token) const;
    void delete_cache(const std::string& db_path, const std::string& mod_folder) const;

    // Ensure the mod_fingerprints table exists
    void ensure_schema(const std::string& db_path) const;

    std::string mods_path_;
    std::unordered_set<std::string> conflict_extensions_;
    std::unordered_set<std::string> ignored_files_;

    // mod_folder -> set of relative paths with conflict-relevant extensions
    std::unordered_map<std::string, std::unordered_set<std::string>> mod_files_;

    // Built conflict index: relative_path -> [(mod_folder, priority)]
    ConflictMap conflicts_;

    // Priority map: mod_folder -> priority (from list position)
    std::unordered_map<std::string, int> priorities_;
};

}  // namespace engine
