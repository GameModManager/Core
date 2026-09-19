#include "engine/gmmpack/uuid.h"

#include <iomanip>
#include <random>
#include <sstream>

namespace engine {

std::string generate_uuid_v4() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dist;
  uint64_t hi = dist(gen);
  uint64_t lo = dist(gen);
  // Version 4 + RFC 4122 variant bits.
  hi = (hi & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;
  lo = (lo & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;

  std::ostringstream ss;
  ss << std::hex << std::setfill('0') << std::setw(8) << (hi >> 32) << "-"
     << std::setw(4) << ((hi >> 16) & 0xffff) << "-" << std::setw(4) << (hi & 0xffff)
     << "-" << std::setw(4) << (lo >> 48) << "-" << std::setw(12)
     << (lo & 0xffffffffffffULL);
  return ss.str();
}

}  // namespace engine
