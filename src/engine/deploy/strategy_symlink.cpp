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

    // Executables and scripts must be REAL files, not symlinks: they resolve
    // siblings relative to their own location, and a symlink resolves through
    // to the mod folder (so skse64_loader.exe would look for SkyrimSE.exe in
    // the mod folder). Same contract as OverlayFsDeployStrategy.
    if (is_executable_binary(source)) {
        std::filesystem::copy_file(source, merged,
                                   std::filesystem::copy_options::overwrite_existing,
                                   ec);
        if (ec) return false;
        std::error_code perm_ec;
        auto perms = std::filesystem::status(merged, perm_ec).permissions();
        if (!perm_ec && (perms & std::filesystem::perms::owner_exec) == std::filesystem::perms::none) {
            std::filesystem::permissions(merged,
                perms | std::filesystem::perms::owner_exec
                      | std::filesystem::perms::group_exec
                      | std::filesystem::perms::others_exec, perm_ec);
        }
        return !perm_ec;
    }

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
