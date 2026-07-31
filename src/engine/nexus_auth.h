#pragma once

#include "engine/keyring.h"

#include <filesystem>
#include <memory>
#include <string>

namespace engine {

// Nexus API rate limit state - persisted across restarts.
struct RateLimitInfo {
    int limit = 0;         // total daily allowance
    int remaining = 0;     // remaining requests
    int64_t reset = 0;     // Unix timestamp when the counter resets
    int64_t last_updated = 0; // Unix timestamp of last API call
};

// Manages Nexus Mods API key storage. Secrets live in the OS keyring
// (injected via set_keyring()); when no OS keyring is available, storage
// falls back to FileKeyring (obfuscated file, insecure) with a warning.
class NexusAuth {
public:
    static NexusAuth& instance();

    bool has_api_key() const;
    std::string get_api_key() const;
    void set_api_key(const std::string& key);
    void clear_api_key();

    // Injects the OS-backed keyring. Call once at startup (before first use).
    void set_keyring(std::unique_ptr<Keyring> keyring);

    // Rate-limit tracking - persisted to disk, survives relaunch.
    RateLimitInfo get_rate_limit() const;
    void update_rate_limit(int limit, int remaining, int64_t reset);

    // Paths
    static std::filesystem::path config_dir();

private:
    NexusAuth();

    // The backend actually used: the injected keyring when available and
    // reachable, otherwise the file fallback.
    Keyring& effective_keyring() const;

    mutable std::unique_ptr<Keyring> keyring_;
    mutable FileKeyring fallback_;
};

} // namespace engine
