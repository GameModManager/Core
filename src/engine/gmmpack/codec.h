#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace engine::gmmpack {

// Strict RFC 4648 base64 (standard alphabet, '=' padding). The decoder
// rejects any character outside the alphabet - a malformed payloadBase64 in
// a patch entry is a corrupt pack, not something to guess at.
inline std::string base64_encode(const uint8_t *data, size_t len) {
  static constexpr char kAlpha[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    int bits   = 1;
    if (i + 1 < len) {
      n |= static_cast<uint32_t>(data[i + 1]) << 8;
      ++bits;
    }
    if (i + 2 < len) {
      n |= data[i + 2];
      ++bits;
    }
    out.push_back(kAlpha[(n >> 18) & 0x3F]);
    out.push_back(kAlpha[(n >> 12) & 0x3F]);
    out.push_back(bits > 1 ? kAlpha[(n >> 6) & 0x3F] : '=');
    out.push_back(bits > 2 ? kAlpha[n & 0x3F] : '=');
  }
  return out;
}

inline std::string base64_encode(const std::vector<uint8_t> &v) {
  return base64_encode(v.data(), v.size());
}

// Returns false (leaving `out` untouched) on any invalid input.
inline bool base64_decode(const std::string &in, std::vector<uint8_t> &out) {
  static constexpr signed char kRev[128] = {
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, 62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60,
      61, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
      11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1,
      -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42,
      43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1};
  if (in.empty()) {
    out.clear();
    return true;
  }
  if (in.size() % 4 != 0)
    return false;
  std::vector<uint8_t> tmp;
  tmp.reserve((in.size() / 4) * 3);
  for (size_t i = 0; i < in.size(); i += 4) {
    unsigned char c0 = in[i], c1 = in[i + 1], c2 = in[i + 2], c3 = in[i + 3];
    if (c0 >= 128 || c1 >= 128 || kRev[c0] < 0 || kRev[c1] < 0)
      return false;
    // Padding may only appear as the last 1-2 chars of the whole input.
    bool end = (i + 4 == in.size());
    if (!end && (c2 == '=' || c3 == '='))
      return false;
    if (c2 != '=' && (c2 >= 128 || kRev[c2] < 0))
      return false;
    if (c3 != '=' && (c3 >= 128 || kRev[c3] < 0))
      return false;
    if (c2 == '=') {
      if (c3 != '=')
        return false;  // padding must be the last 1-2 chars
      tmp.push_back(static_cast<uint8_t>((kRev[c0] << 2) | (kRev[c1] >> 4)));
    } else if (c3 == '=') {
      uint32_t n = (kRev[c0] << 18) | (kRev[c1] << 12) | (kRev[c2] << 6);
      tmp.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
      tmp.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
    } else {
      uint32_t n = (kRev[c0] << 18) | (kRev[c1] << 12) | (kRev[c2] << 6) | kRev[c3];
      tmp.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
      tmp.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
      tmp.push_back(static_cast<uint8_t>(n & 0xFF));
    }
  }
  out = std::move(tmp);
  return true;
}

}  // namespace engine::gmmpack
