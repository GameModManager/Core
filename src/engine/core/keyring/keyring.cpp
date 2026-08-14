#include "engine/core/keyring/keyring.h"

#include "engine/core/log/logger.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <fstream>

namespace engine {

namespace {

constexpr const char* kLegacyFile = "nexus_auth.dat";

// Name -> file stem mapping. Non-alphanumeric chars become '_'.
std::string sanitize(const std::string& name) {
    std::string out = name;
    for (auto& c : out) {
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = '_';
    }
    return out;
}

void warn_insecure_storage() {
    static bool warned = false;
    if (!warned) {
        warned = true;
        Logger::instance().error(
            "FileKeyring: no OS keyring available; storing secrets in an "
            "obfuscated file that is recoverable from the binary (insecure)");
    }
}

} // namespace

FileKeyring::FileKeyring(std::filesystem::path config_dir)
    : config_dir_(std::move(config_dir)) {}

// -----------------------------------------------------------------------
// Machine ID (Linux only; fallback on other platforms)
// -----------------------------------------------------------------------

std::string FileKeyring::machine_id() {
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
// Key derivation - XOR key from machine ID + app salt
// -----------------------------------------------------------------------

std::string FileKeyring::derive_key() {
    std::string seed = machine_id() + "::GMM_NEXUS_2026_SALT";
    std::string key(64, '\0');
    for (size_t i = 0; i < seed.size(); ++i) {
        uint8_t b = static_cast<uint8_t>(seed[i]);
        key[i % 64] ^= static_cast<char>(b);
        b = static_cast<uint8_t>((b << 3) | (b >> 5));
        key[(i + 7) % 64] ^= static_cast<char>(b);
        key[(i + 13) % 64] ^= static_cast<char>(~b);
    }
    return key;
}

// -----------------------------------------------------------------------
// Base64 (RFC 4648) - minimal, no external dependency
// -----------------------------------------------------------------------

static const char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string FileKeyring::base64_encode(const std::string& in) {
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

std::string FileKeyring::base64_decode(const std::string& in) {
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
            if (buf[n] == 0xFF) { n = 0; break; }
        }
        if (n == 0) break;
        if (n < 4) {
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

std::string FileKeyring::encrypt(const std::string& plaintext) {
    std::string key = derive_key();
    std::string xored = plaintext;
    for (size_t i = 0; i < xored.size(); ++i)
        xored[i] ^= key[i % key.size()];
    return base64_encode(xored);
}

std::string FileKeyring::decrypt(const std::string& ciphertext) {
    std::string key = derive_key();
    std::string xored = base64_decode(ciphertext);
    for (size_t i = 0; i < xored.size(); ++i)
        xored[i] ^= key[i % key.size()];
    return xored;
}

// -----------------------------------------------------------------------
// Public storage API
// -----------------------------------------------------------------------

std::filesystem::path FileKeyring::file_for(const std::string& name) const {
    return config_dir_ / ("keyring_" + sanitize(name) + ".dat");
}

bool FileKeyring::has(const std::string& name) const {
    std::error_code ec;
    return std::filesystem::exists(file_for(name), ec);
}

std::string FileKeyring::get(const std::string& name) const {
    std::ifstream f(file_for(name));
    if (!f) return {};
    std::string encrypted;
    f >> encrypted;
    if (encrypted.empty()) return {};
    return decrypt(encrypted);
}

bool FileKeyring::set(const std::string& name, const std::string& value) {
    if (value.empty()) {
        remove(name);
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(config_dir_, ec);
    if (ec) return false;

    warn_insecure_storage();
    std::ofstream f(file_for(name), std::ios::trunc);
    if (!f) return false;
    f << encrypt(value) << std::endl;
    return true;
}

void FileKeyring::remove(const std::string& name) {
    std::error_code ec;
    std::filesystem::remove(file_for(name), ec);
}

std::string FileKeyring::read_legacy(const std::filesystem::path& config_dir) {
    std::ifstream f(config_dir / kLegacyFile);
    if (!f) return {};
    std::string encrypted;
    f >> encrypted;
    if (encrypted.empty()) return {};
    return decrypt(encrypted);
}

void FileKeyring::remove_legacy(const std::filesystem::path& config_dir) {
    std::error_code ec;
    std::filesystem::remove(config_dir / kLegacyFile, ec);
}

} // namespace engine
