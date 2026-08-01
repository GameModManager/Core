#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine {

struct WorkshopItem {
    int64_t workshop_id = 0;
    std::string title;
    std::string preview_url;
    std::string description;
    double created_at = 0;
    double updated_at = 0;
    std::string status;  // "ok", "dead"
};

// Steam Workshop integration for Isaac mods.
//
// Fetches mod details (title, description, dates, preview thumbnails)
// from the Steam Web API, caches them in SQLite, tracks permanently
// removed/failed Workshop IDs, and rate-limits requests.
class WorkshopClient {
public:
    explicit WorkshopClient(const std::string& db_path,
                            int rate_limit = 60, int rate_window = 3600);

    // Fetch details for a Workshop ID. Returns cached data if available.
    // Returns nullopt if the ID is known-dead or on network failure.
    std::optional<WorkshopItem> get_details(int64_t workshop_id);

    // Mark a Workshop ID as permanently dead (removed from Workshop).
    void mark_dead(int64_t workshop_id);

    // Check if a Workshop ID is known-dead.
    [[nodiscard]] bool is_dead(int64_t workshop_id) const;

    // Get all dead Workshop IDs.
    [[nodiscard]] std::vector<int64_t> dead_ids() const;

    // Rate limiter state: returns (requests_in_window, next_available_timestamp)
    [[nodiscard]] std::pair<int, std::optional<double>> rate_limit_state() const;

    // Check if the rate limit allows another request.
    [[nodiscard]] bool can_request() const;

    // Update the rate limit (requests per window) at runtime.
    void set_rate_limit(int limit, int window);

    // Get cached item from SQLite (no network).
    std::optional<WorkshopItem> get_cached(int64_t workshop_id) const;

private:
    void ensure_schema();
    std::optional<WorkshopItem> fetch_from_steam(int64_t workshop_id);
    void save_to_cache(const WorkshopItem& item);
    void load_dead_ids();

    std::string db_path_;
    mutable std::mutex mutex_;

    // In-memory cache: workshop_id -> item
    std::unordered_map<int64_t, WorkshopItem> cache_;

    // Dead IDs (permanently failed)
    std::unordered_set<int64_t> dead_ids_;

    // Rate limiting: defaults 60 requests per hour, configurable.
    std::vector<double> request_timestamps_;
    int rate_limit_ = 60;          // requests per window
    int rate_window_ = 3600;       // seconds (1 window)
    static constexpr int RETRY_COOLDOWN = 3600;   // seconds before retrying a failed ID
};

}  // namespace engine
