#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <string>

namespace engine {

// Generic remote data cache with fetch/cache/bundled/fallback chain.
//
// Fetch order:
//   1. In-memory cache (instant)
//   2. Fresh on-disk cache (within TTL)
//   3. HTTP fetch from URL
//   4. Stale on-disk cache (if network fails)
//   5. Bundled fallback file (shipped with the app)
//   6. Caller-provided default value
class RemoteCache {
public:
    using ParseFn = std::function<void*(const std::string& raw)>;
    using FreeFn = std::function<void*(void*)>;

    RemoteCache(const std::string& url,
                const std::string& cache_path,
                const std::string& bundled_path,
                std::chrono::seconds ttl,
                const std::string& user_agent = "GameModManager/1.0");

    ~RemoteCache();

    // Get data: tries cache → fetch → bundled → fallback. Thread-safe.
    // Returns the parsed data, or nullptr on total failure.
    void* get();

    // Background fetch: returns true if new data was fetched, false on error,
    // nullopt if cache was still fresh.
    std::optional<bool> fetch_background();

    // Force refresh on next get()
    void invalidate();

    // Set the parse function (called on raw text)
    void set_parse(std::function<void*(const std::string&)> fn);

    // Set the free function (for cleanup)
    void set_free(std::function<void(void*)> fn);

private:
    bool is_fresh() const;
    void* try_fetch();
    void* try_cache();
    void* try_bundled();

    std::string url_;
    std::string cache_path_;
    std::string bundled_path_;
    std::chrono::seconds ttl_;
    std::string user_agent_;

    std::function<void*(const std::string&)> parse_fn_;
    std::function<void(void*)> free_fn_;

    mutable std::mutex mutex_;
    void* data_ = nullptr;
};

}  // namespace engine
