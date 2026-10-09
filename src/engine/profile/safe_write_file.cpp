#include "engine/profile/safe_write_file.h"

#include "platform/platform.h"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace engine::profile {

namespace {

  // Unique temp-file suffix: pid + monotonic counter. Two writers on the same
  // target never collide on the temp name, and a stale temp from a crashed
  // process is simply truncated by the next write.
  std::string unique_suffix() {
    static std::atomic<uint64_t> counter{0};
    return std::to_string(current_process_id()) + "_" +
           std::to_string(counter.fetch_add(1));
  }

}  // namespace

bool safe_write_file(const std::filesystem::path &target, const std::string &content) {
  std::error_code ec;
  const auto parent = target.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return false;
    }
  }

  // Temp file lives in the same directory as the target so the final rename
  // stays on one filesystem (rename across devices fails with EXDEV).
  std::filesystem::path temp = target;
  temp += ".tmp" + unique_suffix();

  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    if (!out) {
      out.close();
      std::filesystem::remove(temp, ec);
      return false;
    }
    out.close();
  }

  if (!atomic_replace(temp, target)) {
    std::filesystem::remove(temp, ec);
    return false;
  }
  return true;
}

}  // namespace engine::profile