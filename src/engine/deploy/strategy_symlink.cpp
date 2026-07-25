#include "engine/deploy/strategy.h"

#include <filesystem>

namespace engine {

bool SymlinkStrategy::deploy(const std::filesystem::path& source,
                             const std::filesystem::path& target) {
    std::error_code ec;

    // Remove existing symlink/file at target
    if (std::filesystem::exists(target) || std::filesystem::is_symlink(target)) {
        std::filesystem::remove_all(target, ec);
    }

    // Create parent directories
    std::filesystem::create_directories(target.parent_path(), ec);

    std::filesystem::create_symlink(source, target, ec);
    return !ec;
}

bool SymlinkStrategy::remove(const std::filesystem::path& target) {
    std::error_code ec;
    if (std::filesystem::is_symlink(target)) {
        std::filesystem::remove(target, ec);
        return !ec;
    }
    return false;
}

}  // namespace engine
