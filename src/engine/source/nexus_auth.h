#pragma once

#include "engine/core/keyring/keyring.h"

#include <filesystem>
#include <memory>
#include <string>

namespace engine {

// Nexus API rate limit state - persisted across restarts.
// Nexus reports two independent budgets (see MO2 NexusInterface::parseLimits):
// an hourly one and a daily one, each with its own reset timestamp.
struct RateLimitInfo {
    int hourly_limit = 0;        // total hourly allowance
    int hourly_remaining = 0;    // requests left in the current hour
    int64_t hourly_reset = 0;    // Unix timestamp when the hourly counter resets
    int daily_limit = 0;         // total daily allowance
    int daily_remaining = 0;     // requests left today
    int64_t daily_reset = 0;     // Unix timestamp when the daily counter resets
    int64_t last_updated = 0;    // Unix timestamp of last API call
};

// Nexus account information from users/validate.json - persisted so the
// settings panel can show it without re-validating on every open. MO2 only
// knows Regular/Premium; Nexus additionally exposes a Supporter tier which we
// surface between the two.
struct NexusUserInfo {
    enum class AccountType { None, Regular, Supporter, Premium };
    std::string user_id;
    std::string name;
    AccountType account_type = AccountType::None;
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
    void update_rate_limit(int hourly_limit, int hourly_remaining, int64_t hourly_reset,
                           int daily_limit, int daily_remaining, int64_t daily_reset);

    // User account info from users/validate.json - persisted to disk.
    bool has_user_info() const;
    NexusUserInfo get_user_info() const;
    void set_user_info(const NexusUserInfo& info);
    void clear_user_info();

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
