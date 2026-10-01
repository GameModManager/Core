#include "engine/core/keyring/keyring.h"

#include "engine/core/log/logger.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <random>

#ifdef _WIN32
#include <windows.h>
#endif

namespace engine {

namespace {

  constexpr const char *kLegacyFile = "nexus_auth.dat";

  // Deliberately not a keyring_*.dat name: a blob is always
  // keyring_<sanitized-name>.dat, and sanitize() maps every non-alphanumeric
  // char to '_', so a name can never produce a leading dot. That makes this
  // path collision-proof against a secret literally named "seed".
  constexpr const char *kSeedFile = ".keyring_seed";

  // The key material builds predating keyring_seed.dat used when machine-id
  // was unreadable. Every such install shared it, and it is in the binary, so
  // it is only ever used to READ a blob written then - never to create one.
  constexpr const char *kLegacySeed = "gmm-generic-seed-2024";

  // Name -> file stem mapping. Non-alphanumeric chars become '_'.
  std::string sanitize(const std::string &name) {
    std::string out = name;
    for (auto &c : out) {
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

}  // namespace

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
    if (!id.empty())
      return id;
  }
  {
    std::ifstream f("/var/lib/dbus/machine-id");
    std::string id;
    f >> id;
    if (!id.empty())
      return id;
  }
#elif defined(_WIN32)
  // Windows: read MachineGuid from the registry
  HKEY hkey;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0,
                    KEY_READ, &hkey) == ERROR_SUCCESS) {
    wchar_t buf[256];
    DWORD buf_size = sizeof(buf);
    DWORD type     = REG_SZ;
    if (RegQueryValueExW(hkey, L"MachineGuid", nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf), &buf_size) == ERROR_SUCCESS &&
        type == REG_SZ) {
      RegCloseKey(hkey);
      std::wstring guid(buf, buf_size / sizeof(wchar_t));
      // Convert wide string to narrow for hashing
      std::string narrow;
      narrow.reserve(guid.size());
      for (wchar_t wc : guid)
        narrow += static_cast<char>(wc);
      return narrow;
    }
    RegCloseKey(hkey);
  }
#endif
  return {};
}

// -----------------------------------------------------------------------
// Per-install seed - 16 random bytes, hex, owner-only
// -----------------------------------------------------------------------

// -----------------------------------------------------------------------
// Key derivation - XOR key from seed + app salt
// -----------------------------------------------------------------------

std::string FileKeyring::derive_key(const std::string &seed) {
  std::string salted = seed + "::GMM_NEXUS_2026_SALT";
  std::string key(64, '\0');
  for (size_t i = 0; i < salted.size(); ++i) {
    uint8_t b = static_cast<uint8_t>(salted[i]);
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

std::string FileKeyring::base64_encode(const std::string &in) {
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

std::string FileKeyring::base64_decode(const std::string &in) {
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
      if (c == '=')
        break;
      buf[n] = rev[static_cast<uint8_t>(c)];
      if (buf[n] == 0xFF) {
        n = 0;
        break;
      }
    }
    if (n == 0)
      break;
    if (n < 4) {
      for (size_t j = n; j < 4; ++j)
        buf[j] = 0;
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

std::string FileKeyring::encrypt_with(const std::string &seed,
                                      const std::string &plaintext) {
  const std::string key = derive_key(seed);
  std::string xored     = plaintext;
  for (size_t i = 0; i < xored.size(); ++i)
    xored[i] ^= key[i % key.size()];
  return base64_encode(xored);
}

std::string FileKeyring::decrypt_with(const std::string &seed,
                                      const std::string &ciphertext) {
  const std::string key = derive_key(seed);
  std::string xored     = base64_decode(ciphertext);
  for (size_t i = 0; i < xored.size(); ++i)
    xored[i] ^= key[i % key.size()];
  return xored;
}

// -----------------------------------------------------------------------
// Public storage API
// -----------------------------------------------------------------------

std::filesystem::path FileKeyring::file_for(const std::string &name) const {
  return config_dir_ / ("keyring_" + sanitize(name) + ".dat");
}

// A blob written by an older build is still world-readable until something
// reads it back, so tightening happens on the read path too - not only on
// write, which would leave every existing install exposed.
void FileKeyring::restrict(const std::filesystem::path &path) {
  std::error_code ec;
  std::filesystem::permissions(
      path,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      ec);
  if (ec) {
    Logger::instance().warn("FileKeyring: could not restrict permissions on " +
                            path.filename().string() + ": " + ec.message());
  }
}

bool FileKeyring::write_seed(const std::filesystem::path &dir,
                             const std::string &seed) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec)
    return false;
  return write_blob(dir / kSeedFile, seed);
}

std::string FileKeyring::load_or_create_seed(const std::filesystem::path &dir) {
  const auto path = dir / kSeedFile;
  {
    std::ifstream f(path);
    std::string seed;
    f >> seed;
    if (!seed.empty()) {
      restrict(path);
      return seed;
    }
  }

  std::random_device rd;
  static const char hex[] = "0123456789abcdef";
  std::string seed;
  seed.reserve(32);
  for (int i = 0; i < 16; ++i) {
    const auto b = static_cast<unsigned char>(rd());
    seed += hex[b >> 4];
    seed += hex[b & 0x0F];
  }
  if (!write_seed(dir, seed)) {
    Logger::instance().error(
        "FileKeyring: could not write the per-install key seed; falling back "
        "to the shared legacy seed (secrets are recoverable from the binary)");
    return kLegacySeed;
  }
  return seed;
}

std::string FileKeyring::legacy_seed() {
  const std::string mid = machine_id();
  return mid.empty() ? std::string(kLegacySeed) : mid;
}

bool FileKeyring::has(const std::string &name) const {
  std::error_code ec;
  return std::filesystem::exists(file_for(name), ec);
}

std::string FileKeyring::get(const std::string &name) const {
  const auto path = file_for(name);
  std::ifstream f(path);
  if (!f)
    return {};
  std::string encrypted;
  f >> encrypted;
  if (encrypted.empty())
    return {};
  f.close();

  // No seed file means this blob predates them, so its key is known exactly -
  // no guesswork, and the old key never gets reused for a new blob.
  std::error_code ec;
  if (!std::filesystem::exists(config_dir_ / kSeedFile, ec)) {
    const std::string plaintext = decrypt_with(legacy_seed(), encrypted);
    if (!plaintext.empty()) {
      // Re-encrypt under a fresh per-install seed: the readable machine-id (or
      // the shared constant) stops being the key from here on.
      const std::string seed = load_or_create_seed(config_dir_);
      write_blob(path, encrypt_with(seed, plaintext));
    }
    return plaintext;
  }

  restrict(path);
  return decrypt_with(load_or_create_seed(config_dir_), encrypted);
}

bool FileKeyring::write_blob(const std::filesystem::path &path,
                             const std::string &ciphertext) {
  {
    std::ofstream f(path, std::ios::trunc);
    if (!f)
      return false;
    f << ciphertext << std::endl;
  }  // closed before restrict(): a still-open fd would be re-created wider
  restrict(path);
  return true;
}

bool FileKeyring::set(const std::string &name, const std::string &value) {
  if (value.empty()) {
    remove(name);
    return true;
  }
  std::error_code ec;
  std::filesystem::create_directories(config_dir_, ec);
  if (ec)
    return false;
  // The blobs and the seed in here are secrets; the config dir is this user's
  // own, so owner-only is the correct mode for it too.
  std::filesystem::permissions(config_dir_, std::filesystem::perms::owner_all,
                               ec);
  if (ec) {
    Logger::instance().warn("FileKeyring: could not restrict permissions on " +
                            config_dir_.filename().string() + ": " +
                            ec.message());
  }

  warn_insecure_storage();
  return write_blob(file_for(name),
                    encrypt_with(load_or_create_seed(config_dir_), value));
}

void FileKeyring::remove(const std::string &name) {
  std::error_code ec;
  std::filesystem::remove(file_for(name), ec);
}

std::string FileKeyring::read_legacy(const std::filesystem::path &config_dir) {
  const auto path = config_dir / kLegacyFile;
  std::ifstream f(path);
  if (!f)
    return {};
  std::string encrypted;
  f >> encrypted;
  if (encrypted.empty())
    return {};
  f.close();
  restrict(path);

  std::error_code ec;
  if (std::filesystem::exists(config_dir / kSeedFile, ec))
    return decrypt_with(load_or_create_seed(config_dir), encrypted);
  return decrypt_with(legacy_seed(), encrypted);
}

void FileKeyring::remove_legacy(const std::filesystem::path &config_dir) {
  std::error_code ec;
  std::filesystem::remove(config_dir / kLegacyFile, ec);
}

}  // namespace engine
