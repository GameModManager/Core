#pragma once

#include "engine/deploy/strategy.h"

#include <string>
#include <vector>
#include <mutex>

namespace engine {

// OverlayFS deploy strategy — O(1) reorder via remount
// Requires Linux kernel with overlayfs support and appropriate privileges
class OverlayFsStrategy : public DeploymentStrategy {
public:
    OverlayFsStrategy();
    ~OverlayFsStrategy() override;

    bool deploy(const std::filesystem::path& source,
                const std::filesystem::path& target) override;
    bool remove(const std::filesystem::path& target) override;

    // Mount an overlay at mount_point with the given lower directories (ordered by priority)
    bool mount(const std::filesystem::path& mount_point,
               const std::vector<std::filesystem::path>& lower_dirs);

    // Remount with a new lowerdir order (O(1) reorder)
    bool remount(const std::vector<std::filesystem::path>& lower_dirs);

    bool unmount();

    [[nodiscard]] bool is_mounted() const { return mounted_; }

    // Check if overlayfs is available and usable
    [[nodiscard]] static bool is_available();

    // Check if we can mount without root (unprivileged overlayfs via fusermount3)
    [[nodiscard]] static bool can_mount_unprivileged();

private:
    bool mounted_ = false;
    std::filesystem::path mount_point_;
    std::filesystem::path work_dir_;   // required by overlayfs (empty upper dir for read-only overlay)
    mutable std::mutex mutex_;
};

}  // namespace engine
