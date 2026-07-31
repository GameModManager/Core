#include "engine/nexus_auth.h"

#include "engine/log/logger.h"

#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

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

    // Minimal JSON parse (no nlohmann dependency in the engine level for auth)
    // Format: {"limit":N,"remaining":N,"reset":N,"last_updated":N}
    auto read_int = [&](const std::string& key) -> int {
        auto pos = buf.str().find("\"" + key + "\"");
        if (pos == std::string::npos) return 0;
        pos = buf.str().find(':', pos);
        if (pos == std::string::npos) return 0;
        ++pos;
        while (pos < buf.str().size() && (buf.str()[pos] == ' ' || buf.str()[pos] == '\t'))
            ++pos;
        int sign = 1;
        if (buf.str()[pos] == '-') { sign = -1; ++pos; }
        int val = 0;
        while (pos < buf.str().size() && buf.str()[pos] >= '0' && buf.str()[pos] <= '9') {
            val = val * 10 + (buf.str()[pos] - '0');
            ++pos;
        }
        return val * sign;
    };

    info.limit = read_int("limit");
    info.remaining = read_int("remaining");
    info.reset = read_int("reset");
    info.last_updated = read_int("last_updated");
    return info;
}

void NexusAuth::update_rate_limit(int limit, int remaining, int64_t reset) {
    std::error_code ec;
    std::filesystem::create_directories(config_dir(), ec);

    std::ofstream f(config_dir() / "nexus_rate.json", std::ios::trunc);
    if (!f) return;

    auto now = std::time(nullptr);
    f << "{\n"
      << "  \"limit\": " << limit << ",\n"
      << "  \"remaining\": " << remaining << ",\n"
      << "  \"reset\": " << reset << ",\n"
      << "  \"last_updated\": " << now << "\n"
      << "}\n";
}

} // namespace engine
