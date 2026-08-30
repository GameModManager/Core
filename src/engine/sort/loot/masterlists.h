#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace engine {

class Platform;

// Per-game LOOT masterlist + prelude management (PLAN.md §7.1). The engine owns
// masterlists so gmm_lootcli stays networking-free: files land in
// <data_dir>/loot/<game_id>/{masterlist.yaml,prelude.yaml}, fetched from the
// game's official loot/<repo> GitHub repo with a version-branch walk-down
// (newest first, then "master"), re-downloaded at most once per TTL.
// No-network fallback: a previously cached copy is kept and reused.
class MasterlistManager {
public:
    explicit MasterlistManager(const Platform* platform);

    struct Masterlists {
        std::filesystem::path masterlist;
        std::filesystem::path prelude;
        bool cached = false;  // true = on-disk copies reused, no network used
    };

    // Make masterlists available for `game_id` and return their paths. `repo`
    // is the loot/<repo> GitHub slug (e.g. "skyrimse"). When no branch has a
    // masterlist and no cached copy exists, both paths come back empty and
    // *error (if given) explains why.
    Masterlists ensure(const std::string& game_id,
                       const std::string& repo,
                       std::string* error = nullptr);

    // Force a re-download regardless of TTL (explicit "update masterlists").
    // Falls back to cache when offline, same as ensure().
    Masterlists update(const std::string& game_id,
                       const std::string& repo,
                       std::string* error = nullptr);

    // Storage dir for a game's masterlists (created on demand).
    std::filesystem::path dir_for(const std::string& game_id) const;

    // Version branches probed in order (newest first); "master" is the final
    // fallback. Mirrors Amethyst's walk-down floor (v0.21 is the oldest branch
    // any currently supported game uses) with MO2's oldDefaultBranches "master"
    // sentinel. Exposed for tests.
    static const std::vector<std::string>& branch_candidates();

    // Masterlists are re-downloaded at most once per refresh interval.
    static constexpr auto kRefreshAfter = std::chrono::hours(24);

private:
    Masterlists fetch_or_cache(const std::string& game_id,
                               const std::string& repo,
                               bool force,
                               std::string* error);

    const Platform* platform_;
};

}  // namespace engine
