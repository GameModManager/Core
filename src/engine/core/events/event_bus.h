#pragma once

// P1.3 plugin event bus (PLAN.md §19.4 P1.3). Qt-free, thread-safe — a plugin
// subscribes through the subscribe_event ABI (or its pybind mirror) and the
// host emits into it at the existing mod/plugin/profile signal points. Mirrors
// MO2's boost::signals2 plugin-facing subscription surface (onModInstalled,
// onModStateChanged, onProfileChanged, ... — REFERENCES/modorganizer/src/
// {modlist,pluginlist,organizercore,downloadmanager}.h).
//
// Contract:
//   - Handlers run on the EMITTING thread, AFTER the bus lock is released.
//     (MO2 signals2 and our Logger callbacks have the same calling-thread
//     contract.) A handler may re-emit or unsubscribe without deadlocking.
//   - The payload is a single JSON object string. The host builds it with the
//     json_obj() helper below; a C plugin parses it with its own JSON code, a
//     Python plugin receives a dict (the pybind mirror json.loads it).
//   - Any thread may emit (pipeline worker, scan worker, main thread); the bus
//     never blocks the UI — emission is a vector copy + direct calls.

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace engine {

// Canonical event ids — the host emits exactly these; plugins subscribe to
// them by name. Each carries a JSON-object payload (documented per id in
// gmm_abi_v1.h subscribe_event and in the ABI doc header).
namespace events {
inline constexpr const char* kModInstalled = "mod_installed";
inline constexpr const char* kModRemoved = "mod_removed";
inline constexpr const char* kModStateChanged = "mod_state_changed";
inline constexpr const char* kModMoved = "mod_moved";
inline constexpr const char* kProfileCreated = "profile_created";
inline constexpr const char* kProfileRenamed = "profile_renamed";
inline constexpr const char* kProfileRemoved = "profile_removed";
inline constexpr const char* kProfileChanged = "profile_changed";
inline constexpr const char* kPluginListRefreshed = "plugin_list_refreshed";
inline constexpr const char* kPluginStateChanged = "plugin_state_changed";
inline constexpr const char* kPluginMoved = "plugin_moved";
inline constexpr const char* kDownloadComplete = "download_complete";
inline constexpr const char* kDownloadPaused = "download_paused";
inline constexpr const char* kDownloadFailed = "download_failed";
inline constexpr const char* kDownloadRemoved = "download_removed";
inline constexpr const char* kGameLaunched = "game_launched";
inline constexpr const char* kGameFinished = "game_finished";
}  // namespace events

// A single recorded event, retained in a bounded ring buffer so tooling (e.g.
// the Event Bus Viewer plugin) can show recent bus activity without having
// subscribed to every event.
struct EventRecord {
    std::string event_id;
    std::string payload;
    std::chrono::system_clock::time_point timestamp;
};

class EventBus {
public:
    using Handler = std::function<void(const std::string& event_id,
                                       const std::string& payload_json)>;

    static EventBus& instance();

    // Subscribe a handler for event_id. source is the subscribing plugin's
    // path (used by clear_source() on plugin unload/shutdown); empty for
    // engine-internal subscribers. Returns a non-zero token, or 0 when
    // event_id is empty or handler is null.
    uint64_t subscribe(const std::string& event_id, Handler handler,
                       const std::string& source = "");

    // Drop one subscription; false when the token is unknown.
    bool unsubscribe(uint64_t token);

    // Dispatch to every handler subscribed for event_id, in subscription
    // order. Handlers run on the calling thread, never under the bus lock.
    // (Named dispatch, not emit: Qt defines emit as a macro.)
    void dispatch(const std::string& event_id, const std::string& payload_json) const;

    // Drop every subscription registered by source (plugin unload/shutdown).
    void clear_source(const std::string& source);

    // Drop all subscriptions (python shutdown, plugin-container teardown,
    // test teardown).
    void clear();

    [[nodiscard]] size_t subscriber_count(const std::string& event_id) const;

    // Recent dispatched events, most-recent last, capped at kMaxHistory. Used by
    // tooling such as the Event Bus Viewer plugin.
    static constexpr size_t kMaxHistory = 500;
    [[nodiscard]] std::vector<EventRecord> recent_events(
        size_t max = kMaxHistory) const;

private:
    EventBus() = default;

    struct Subscription {
        uint64_t token = 0;
        std::string event_id;
        std::string source;
        Handler handler;
    };

    std::vector<Subscription> subs_;
    mutable std::vector<EventRecord> history_;  // guarded by mutex_
    mutable std::mutex mutex_;
    uint64_t next_token_ = 1;
};

// -- JSON payload helpers (Qt-free; the bus payload is a JSON object string) --

// The payload key/values as an escaped JSON object, e.g.
//   json_obj({{"mod", "SkyUI"}, {"enabled", "1"}})
// -> {"mod":"SkyUI","enabled":"1"}
[[nodiscard]] std::string json_obj(
    std::vector<std::pair<std::string, std::string>> kv);

}  // namespace engine
