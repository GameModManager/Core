#include "engine/nexus_auth.h"

#include "engine/log/logger.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <ctime>
#include <fstream>

namespace engine {

namespace {
constexpr const char* kKeyName = "nexus-api-key";
} // namespace

// -----------------------------------------------------------------------
// Config directory / storage paths
// -----------------------------------------------------------------------

std::filesystem::path NexusAuth::config_dir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        return std::filesystem::path(xdg) / "GameModManager";
    const char* home = std::getenv("HOME");
    if (home)
        return std::filesystem::path(home) / ".config" / "GameModManager";
    return std::filesystem::path("/tmp") / "GameModManager";
}

// -----------------------------------------------------------------------
// Keyring selection
// -----------------------------------------------------------------------

NexusAuth::NexusAuth()
    : fallback_(config_dir()) {}

void NexusAuth::set_keyring(std::unique_ptr<Keyring> keyring) {
    keyring_ = std::move(keyring);
}

Keyring& NexusAuth::effective_keyring() const {
    if (keyring_ && keyring_->available())
        return *keyring_;
    return fallback_;
}

// -----------------------------------------------------------------------
// Public API - API key
// -----------------------------------------------------------------------

NexusAuth& NexusAuth::instance() {
    static NexusAuth inst;
    return inst;
}

bool NexusAuth::has_api_key() const {
    try {
        return effective_keyring().has(kKeyName);
    } catch (const std::exception& e) {
        Logger::instance().error("NexusAuth: keyring has() failed: " +
                                 std::string(e.what()));
        return false;
    }
}

std::string NexusAuth::get_api_key() const {
    try {
        Keyring& kr = effective_keyring();
        if (!kr.available()) {
            Logger::instance().error(
                "NexusAuth: no OS keyring; falling back to insecure file storage");
        }

        // One-time migration: an OS keyring is available but holds no key yet,
        // and a legacy pre-keyring file exists -> move it into the keyring.
        if (kr.available() && !kr.has(kKeyName)) {
            std::string legacy = FileKeyring::read_legacy(config_dir());
            if (!legacy.empty()) {
                if (kr.set(kKeyName, legacy)) {
                    FileKeyring::remove_legacy(config_dir());
                    Logger::instance().info(
                        "NexusAuth: migrated API key from legacy file into OS keyring");
                } else {
                    Logger::instance().error(
                        "NexusAuth: failed to migrate legacy API key into keyring");
                }
            }
        }

        return kr.get(kKeyName);
    } catch (const std::exception& e) {
        Logger::instance().error("NexusAuth: keyring get() failed: " +
                                 std::string(e.what()));
        return {};
    }
}

void NexusAuth::set_api_key(const std::string& key) {
    try {
        Keyring& kr = effective_keyring();
        if (kr.set(kKeyName, key)) {
            // Don't leave an obsolete copy of the secret behind.
            FileKeyring::remove_legacy(config_dir());
            return;
        }
        Logger::instance().error("NexusAuth: failed to store API key in keyring");
    } catch (const std::exception& e) {
        Logger::instance().error("NexusAuth: keyring set() failed: " +
                                 std::string(e.what()));
    }
}

void NexusAuth::clear_api_key() {
    try {
        effective_keyring().remove(kKeyName);
    } catch (const std::exception& e) {
        Logger::instance().error("NexusAuth: keyring remove() failed: " +
                                 std::string(e.what()));
    }
    FileKeyring::remove_legacy(config_dir());
}

// -----------------------------------------------------------------------
// Public API - rate limits
// -----------------------------------------------------------------------

RateLimitInfo NexusAuth::get_rate_limit() const {
    RateLimitInfo info;
    std::ifstream f(config_dir() / "nexus_rate.json");
    if (!f) return info;

    std::stringstream buf;
    buf << f.rdbuf();

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(buf.str());
    } catch (const std::exception&) {
        return info;
    }
    // Format:
    // {"hourly_limit":N,"hourly_remaining":N,"hourly_reset":N,
    //  "daily_limit":N,"daily_remaining":N,"daily_reset":N,"last_updated":N}
    info.hourly_limit = j.value("hourly_limit", 0);
    info.hourly_remaining = j.value("hourly_remaining", 0);
    info.hourly_reset = j.value("hourly_reset", int64_t{0});
    info.daily_limit = j.value("daily_limit", 0);
    info.daily_remaining = j.value("daily_remaining", 0);
    info.daily_reset = j.value("daily_reset", int64_t{0});
    info.last_updated = j.value("last_updated", int64_t{0});

    // Legacy files (pre hourly/daily split) stored the daily budget under
    // limit/remaining/reset - migrate them into the daily fields.
    if (info.daily_limit == 0) info.daily_limit = j.value("limit", 0);
    if (info.daily_remaining == 0) info.daily_remaining = j.value("remaining", 0);
    if (info.daily_reset == 0) info.daily_reset = j.value("reset", int64_t{0});

    return info;
}

void NexusAuth::update_rate_limit(int hourly_limit, int hourly_remaining, int64_t hourly_reset,
                                  int daily_limit, int daily_remaining, int64_t daily_reset) {
    std::error_code ec;
    std::filesystem::create_directories(config_dir(), ec);

    nlohmann::json j = {{"hourly_limit", hourly_limit},
                        {"hourly_remaining", hourly_remaining},
                        {"hourly_reset", hourly_reset},
                        {"daily_limit", daily_limit},
                        {"daily_remaining", daily_remaining},
                        {"daily_reset", daily_reset},
                        {"last_updated", std::time(nullptr)}};

    std::ofstream f(config_dir() / "nexus_rate.json", std::ios::trunc);
    if (!f) return;
    f << j.dump(2) << "\n";
}

// -----------------------------------------------------------------------
// Public API - user account info
// -----------------------------------------------------------------------

bool NexusAuth::has_user_info() const {
    return std::filesystem::exists(config_dir() / "nexus_user.json");
}

NexusUserInfo NexusAuth::get_user_info() const {
    NexusUserInfo info;
    std::ifstream f(config_dir() / "nexus_user.json");
    if (!f) return info;

    std::stringstream buf;
    buf << f.rdbuf();

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(buf.str());
    } catch (const std::exception&) {
        return info;
    }
    // Format:
    // {"user_id":"...","name":"...","account_type":"premium|supporter|regular"}
    info.user_id = j.value("user_id", std::string{});
    info.name = j.value("name", std::string{});
    const std::string type = j.value("account_type", std::string{});
    if (type == "premium")
        info.account_type = NexusUserInfo::AccountType::Premium;
    else if (type == "supporter")
        info.account_type = NexusUserInfo::AccountType::Supporter;
    else if (!type.empty())
        info.account_type = NexusUserInfo::AccountType::Regular;

    return info;
}

void NexusAuth::set_user_info(const NexusUserInfo& info) {
    std::error_code ec;
    std::filesystem::create_directories(config_dir(), ec);

    const char* type = "regular";
    if (info.account_type == NexusUserInfo::AccountType::Premium) type = "premium";
    else if (info.account_type == NexusUserInfo::AccountType::Supporter) type = "supporter";

    nlohmann::json j = {{"user_id", info.user_id},
                        {"name", info.name},
                        {"account_type", type}};

    std::ofstream f(config_dir() / "nexus_user.json", std::ios::trunc);
    if (!f) return;
    f << j.dump(2) << "\n";
}

void NexusAuth::clear_user_info() {
    std::error_code ec;
    std::filesystem::remove(config_dir() / "nexus_user.json", ec);
}

} // namespace engine
