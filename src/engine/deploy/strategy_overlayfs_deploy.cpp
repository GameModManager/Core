#include "engine/deploy/strategy.h"
#include "engine/deploy/deploy_utils.h"
#include "engine/log/logger.h"

#include <filesystem>
#include <system_error>

namespace engine {

OverlayFsDeployStrategy::OverlayFsDeployStrategy(std::filesystem::path staging_dir,
                                                 bool case_sensitive)
    : staging_dir_(std::move(staging_dir)), case_sensitive_(case_sensitive) {}

bool OverlayFsDeployStrategy::deploy(const std::filesystem::path& source,
                                      const std::filesystem::path& target) {
    std::error_code ec;
    const std::filesystem::path merged =
        case_sensitive_ ? target : resolve_deploy_target_ci(target);
    std::filesystem::create_directories(merged.parent_path(), ec);
    if (ec) {
        Logger::instance().error("OverlayFS deploy: failed to create dir " +
            merged.parent_path().string() + ": " + ec.message());
        return false;
    }

    // Redeploys write into the same staging dir on every launch, so the target
    // already exists on the second run.  Clear it first: create_symlink would
    // otherwise fail with EEXIST and a stale entry (from a removed/renamed mod
    // file) would linger in the overlay.
    std::filesystem::remove(merged, ec);
    if (ec) {
        Logger::instance().error("OverlayFS deploy: failed to clear stale target " +
            merged.string() + ": " + ec.message());
        return false;
    }

    std::filesystem::create_symlink(source, merged, ec);
    if (ec) {
        Logger::instance().error("OverlayFS deploy: failed to symlink " +
            merged.string() + " -> " + source.string() + ": " + ec.message());
        return false;
    }
    return true;
}

bool OverlayFsDeployStrategy::remove(const std::filesystem::path& target) {
    std::error_code ec;
    std::filesystem::remove(target, ec);
    return true;
}

void OverlayFsDeployStrategy::set_mod_paths(const std::vector<std::filesystem::path>& paths) {
    mod_paths_ = paths;
}

const std::vector<std::filesystem::path>& OverlayFsDeployStrategy::mod_paths() const {
    return mod_paths_;
}

}  // namespace engine