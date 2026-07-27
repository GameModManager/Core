#pragma once

#include "engine/deploy/strategy.h"

#include <filesystem>

namespace engine {

// Hardlink strategy — invisible to the game, no extra disk usage.
// Cross-volume hardlinks are impossible (NTFS and ext4 both reject them),
// so this checks the source and target share the same device before linking.
// On cross-volume failure, returns false so the pipeline can fall back.
class HardlinkStrategy : public DeploymentStrategy {
public:
    bool deploy(const std::filesystem::path& source,
                const std::filesystem::path& target) override;
    bool remove(const std::filesystem::path& target) override;

    // Check if source and target are on the same filesystem (required for hardlinks).
    [[nodiscard]] static bool same_volume(
        const std::filesystem::path& a, const std::filesystem::path& b);
};

}  // namespace engine
