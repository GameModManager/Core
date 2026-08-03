#include "engine/deploy/strategy.h"
#include "engine/deploy/deploy_utils.h"

#include <filesystem>

namespace engine {

SymlinkStrategy::SymlinkStrategy(bool case_sensitive)
    : case_sensitive_(case_sensitive) {}

bool SymlinkStrategy::deploy(const std::filesystem::path& source,
                             const std::filesystem::path& target) {
    std::error_code ec;
    const std::filesystem::path merged =
        case_sensitive_ ? target : resolve_deploy_target_ci(target);

    // Remove existing symlink/file at target
    if (std::filesystem::exists(merged) || std::filesystem::is_symlink(merged)) {
        std::filesystem::remove_all(merged, ec);
    }

    // Create parent directories
    std::filesystem::create_directories(merged.parent_path(), ec);

    std::filesystem::create_symlink(source, merged, ec);
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
