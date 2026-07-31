#include "engine/deploy/strategy_hardlink.h"

#include <filesystem>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace engine {

bool HardlinkStrategy::same_volume(const std::filesystem::path& a,
                                   const std::filesystem::path& b) {
    std::error_code ec;

#ifdef _WIN32
    // On Windows, compare root paths (drive letters)
    auto root_a = a.root_path();
    auto root_b = b.root_path();
    return root_a == root_b;
#else
    // On POSIX, compare device numbers via stat
    struct stat sa, sb;
    if (stat(a.c_str(), &sa) != 0) return false;
    if (stat(b.c_str(), &sb) != 0) return false;
    return sa.st_dev == sb.st_dev;
#endif
}

bool HardlinkStrategy::deploy(const std::filesystem::path& source,
                              const std::filesystem::path& target) {
    if (!std::filesystem::exists(source)) return false;

    // Hardlinks require same volume/filesystem
    if (!same_volume(source, target)) return false;

    std::error_code ec;

    // Remove existing file at target
    if (std::filesystem::exists(target)) {
        std::filesystem::remove(target, ec);
        if (ec) return false;
    }

    // Create parent directories
    std::filesystem::create_directories(target.parent_path(), ec);

    std::filesystem::create_hard_link(source, target, ec);
    return !ec;
}

bool HardlinkStrategy::remove(const std::filesystem::path& target) {
    std::error_code ec;
    // Only remove regular files (not directories - hardlinks can't create dirs)
    if (std::filesystem::is_regular_file(target)) {
        std::filesystem::remove(target, ec);
        return !ec;
    }
    return false;
}

}  // namespace engine
