#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace engine::Source::Nexus {

// A Nexus download server seen while resolving/downloading a file. Mirrors
// MO2's ServerInfo: preferred > 0 means the server is in the user's Preferred
// list (higher = more preferred), 0 means it's only known. Speed samples are
// kept for the same reason MO2 keeps them - so the UI can show an average and
// the user can pick the fastest mirror.
struct NexusServer {
    std::string name;
    bool premium = false;       // premium-only mirror (name mentions Premium)
    int preferred = 0;          // 0 = known, >0 = preferred rank (higher = better)
    int64_t last_seen = 0;      // Unix day number of last discovery/download
    std::vector<int> speeds;    // bytes/second samples, newest last, capped

    int average_speed() const;  // 0 when no samples
};

// Nexus download-server registry. Qt-free so NexusProvider can read/write it
// from worker threads while the settings UI edits it from the main thread
// (hence the mutex). Persisted as JSON next to the auth/rate-limit files in
// Auth::config_dir(). Mirrors MO2's NetworkSettings::servers().
class Servers {
public:
    static Servers& instance();

    std::vector<NexusServer> all() const;          // storage order
    std::vector<NexusServer> known() const;        // preferred == 0
    std::vector<NexusServer> preferred() const;    // preferred > 0, top rank first
    int preferred_rank(const std::string& name) const;  // 0 when not preferred

    // Discovery: called for every download_link.json entry. New servers are
    // added; the CDN mirror starts preferred (MO2 behavior) so it outranks
    // anonymous mirrors before the user expresses a preference.
    void record_discovered(const std::string& name, bool premium);
    // Called after a served download completes (bps > 0). Adds a rolling
    // sample and marks the server as seen today.
    void record_speed(const std::string& name, double bps);
    // Persist the user's drag&drop result. ordered_names lists the Preferred
    // servers top-first; every server not in it is demoted to known.
    void set_preferred(const std::vector<std::string>& ordered_names);

    // Test hook: drop everything and persist the empty set.
    void clear_all();

private:
    Servers() = default;
    void load_locked() const;
    void save_locked() const;

    // Mutable so the const accessors can lazily populate state while the
    // mutex (also mutable) holds the lock - the registry is a threadsafe
    // singleton regardless of the constness of a given accessor.
    mutable std::mutex mutex_;
    mutable bool loaded_ = false;
    mutable std::vector<NexusServer> servers_;
};

} // namespace engine::Source::Nexus
