#include "engine/deploy/strategy.h"
#include "engine/log/logger.h"

#include <filesystem>
#include <system_error>

namespace engine {

OverlayFsDeployStrategy::OverlayFsDeployStrategy(std::filesystem::path staging_dir)
    : staging_dir_(std::move(staging_dir)) {}

bool OverlayFsDeployStrategy::deploy(const std::filesystem::path& source,
                                      const std::filesystem::path& target) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        Logger::instance().error("OverlayFS deploy: failed to create dir " +
            target.parent_path().string() + ": " + ec.message());
        return false;
    }

    std::filesystem::create_symlink(source, target, ec);
    if (ec) {
        Logger::instance().error("OverlayFS deploy: failed to symlink " +
            target.string() + " -> " + source.string() + ": " + ec.message());
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