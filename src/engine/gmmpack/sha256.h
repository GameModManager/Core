#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::gmmpack {

// Minimal public-domain-style SHA-256, implemented from FIPS 180-4 so the
// patch engine stays dependency-free (no OpenSSL/bundled crypto on any of
// the target platforms). Used to verify baseFileSha256 before applying a
// patch - a mismatch means the upstream mod file changed since the pack was
// authored and the patch must NOT be applied.
std::string sha256_hex(const uint8_t *data, size_t len);

inline std::string sha256_hex(const std::string &s) {
  return sha256_hex(reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

inline std::string sha256_hex(const std::vector<uint8_t> &v) {
  return sha256_hex(v.data(), v.size());
}

}  // namespace engine::gmmpack
