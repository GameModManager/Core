#pragma once

#include <string>
#include <filesystem>

namespace engine {

// Nexus API rate limit state - persisted across restarts.
struct RateLimitInfo {
    int limit = 0;         // total daily allowance
    int remaining = 0;     // remaining requests
    int64_t reset = 0;     // Unix timestamp when the counter resets
    int64_t last_updated = 0; // Unix timestamp of last API call
};

// Manages Nexus Mods API key storage with obfuscated (XOR+b64) persistence.
// Not real crypto - prevents casual plaintext reading of the stored key.
class NexusAuth {
public:
    static NexusAuth& instance();

    bool has_api_key() const;
    std::string get_api_key() const;
    void set_api_key(const std::string& key);
    void clear_api_key();

    // Rate-limit tracking - persisted to disk, survives relaunch.
    RateLimitInfo get_rate_limit() const;
    void update_rate_limit(int limit, int remaining, int64_t reset);

    // Paths
    static std::filesystem::path config_dir();

private:
    NexusAuth() = default;

    std::string derive_key() const;
    std::string encrypt(const std::string& plaintext) const;
    std::string decrypt(const std::string& ciphertext) const;

    static std::string base64_encode(const std::string& in);
    static std::string base64_decode(const std::string& in);
    static std::string machine_id();

    static std::filesystem::path key_storage_path();
    static std::filesystem::path rate_storage_path();
};

} // namespace engine
