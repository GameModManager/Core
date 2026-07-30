#include "engine/nexus_auth.h"

#include <fstream>
#include <sstream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>

namespace engine {

// -----------------------------------------------------------------------
// Machine ID (Linux only; fallback on other platforms)
// -----------------------------------------------------------------------

std::string NexusAuth::machine_id() {
#ifdef __linux__
    {
        std::ifstream f("/etc/machine-id");
        std::string id;
        f >> id;
        if (!id.empty()) return id;
    }
    {
        std::ifstream f("/var/lib/dbus/machine-id");
        std::string id;
        f >> id;
        if (!id.empty()) return id;
    }
#endif
    return "gmm-generic-seed-2024";
}

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

std::filesystem::path NexusAuth::key_storage_path() {
    return config_dir() / "nexus_auth.dat";
}

std::filesystem::path NexusAuth::rate_storage_path() {
    return config_dir() / "nexus_rate.json";
}

// -----------------------------------------------------------------------
// Key derivation — XOR key from machine ID + app salt
// -----------------------------------------------------------------------

std::string NexusAuth::derive_key() const {
    std::string seed = machine_id() + "::GMM_NEXUS_2026_SALT";
    // Produce a 64-byte XOR key via simple mixing
    std::string key(64, '\0');
    for (size_t i = 0; i < seed.size(); ++i) {
        uint8_t b = static_cast<uint8_t>(seed[i]);
        key[i % 64] ^= static_cast<char>(b);
        // Rotate and mix
        b = static_cast<uint8_t>((b << 3) | (b >> 5));
        key[(i + 7) % 64] ^= static_cast<char>(b);
        key[(i + 13) % 64] ^= static_cast<char>(~b);
    }
    return key;
}

// -----------------------------------------------------------------------
// Base64 (RFC 4648) — minimal, no external dependency
// -----------------------------------------------------------------------

static const char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string NexusAuth::base64_encode(const std::string& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    uint8_t buf[3];
    for (size_t i = 0; i < in.size(); i += 3) {
        size_t n = std::min<size_t>(3, in.size() - i);
        std::memset(buf, 0, 3);
        std::memcpy(buf, &in[i], n);
        out += kBase64[buf[0] >> 2];
        out += kBase64[((buf[0] & 0x03) << 4) | (buf[1] >> 4)];
        out += (n > 1) ? kBase64[((buf[1] & 0x0F) << 2) | (buf[2] >> 6)] : '=';
        out += (n > 2) ? kBase64[buf[2] & 0x3F] : '=';
    }
    return out;
}

std::string NexusAuth::base64_decode(const std::string& in) {
    // Build reverse lookup
    uint8_t rev[256];
    std::memset(rev, 0xFF, sizeof(rev));
    for (int i = 0; i < 64; ++i)
        rev[static_cast<uint8_t>(kBase64[i])] = static_cast<uint8_t>(i);

    std::string out;
    out.reserve((in.size() / 4) * 3);
    uint8_t buf[4];
    size_t pos = 0;
    while (pos < in.size()) {
        size_t n = 0;
        for (; n < 4 && pos < in.size(); ++n, ++pos) {
            char c = in[pos];
            if (c == '=') break;
            buf[n] = rev[static_cast<uint8_t>(c)];
            if (buf[n] == 0xFF) { n = 0; break; } // skip invalid
        }
        if (n == 0) break;
        if (n < 4) {
            // Pad remaining with 0
            for (size_t j = n; j < 4; ++j) buf[j] = 0;
        }
        out += static_cast<char>((buf[0] << 2) | (buf[1] >> 4));
        if (n > 2)
            out += static_cast<char>(((buf[1] & 0x0F) << 4) | (buf[2] >> 2));
        if (n > 3)
            out += static_cast<char>(((buf[2] & 0x03) << 6) | buf[3]);
    }
    return out;
}

// -----------------------------------------------------------------------
// Encrypt / Decrypt (XOR with derived key + base64 transport)
// -----------------------------------------------------------------------

std::string NexusAuth::encrypt(const std::string& plaintext) const {
    std::string key = derive_key();
    std::string xored = plaintext;
    for (size_t i = 0; i < xored.size(); ++i)
        xored[i] ^= key[i % key.size()];
    return base64_encode(xored);
}

std::string NexusAuth::decrypt(const std::string& ciphertext) const {
    std::string key = derive_key();
    std::string xored = base64_decode(ciphertext);
    for (size_t i = 0; i < xored.size(); ++i)
        xored[i] ^= key[i % key.size()];
    return xored;
}

// -----------------------------------------------------------------------
// Public API — API key
// -----------------------------------------------------------------------

NexusAuth& NexusAuth::instance() {
    static NexusAuth inst;
    return inst;
}

bool NexusAuth::has_api_key() const {
    std::error_code ec;
    return std::filesystem::exists(key_storage_path(), ec);
}

std::string NexusAuth::get_api_key() const {
    std::ifstream f(key_storage_path());
    if (!f) return {};
    std::string encrypted;
    f >> encrypted;
    if (encrypted.empty()) return {};
    return decrypt(encrypted);
}

void NexusAuth::set_api_key(const std::string& key) {
    std::error_code ec;
    std::filesystem::create_directories(config_dir(), ec);

    std::ofstream f(key_storage_path(), std::ios::trunc);
    if (!f) return;
    f << encrypt(key) << std::endl;
}

void NexusAuth::clear_api_key() {
    std::error_code ec;
    std::filesystem::remove(key_storage_path(), ec);
}

// -----------------------------------------------------------------------
// Public API — rate limits
// -----------------------------------------------------------------------

RateLimitInfo NexusAuth::get_rate_limit() const {
    RateLimitInfo info;
    std::ifstream f(rate_storage_path());
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

    std::ofstream f(rate_storage_path(), std::ios::trunc);
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
