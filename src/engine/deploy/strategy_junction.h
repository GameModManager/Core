#pragma once

#include "engine/deploy/strategy.h"

#include <filesystem>

namespace engine {

// NTFS Junction Strategy — directory-level linking on Windows.
// Junction points are preferred over symlinks on Windows because they don't
// require admin privileges or Developer Mode. They only work for directories.
// For individual files, falls back to copy.
class JunctionStrategy : public DeploymentStrategy {
public:
    bool deploy(const std::filesystem::path& source,
                const std::filesystem::path& target) override;
    bool remove(const std::filesystem::path& target) override;

    // Check if the current platform supports junction points.
    [[nodiscard]] static bool is_available();
};

}  // namespace engine
