#pragma once

#include "engine/deploy/strategy.h"

#include <string>
#include <unordered_map>
#include <mutex>

namespace engine {

// FUSE-based VFS deploy strategy
// Creates a virtual filesystem that overlays mod files on top of the game directory
// Only available on Linux with libfuse
class VfsStrategy : public DeploymentStrategy {
public:
    VfsStrategy();
    ~VfsStrategy() override;

    bool deploy(const std::filesystem::path& source,
                const std::filesystem::path& target) override;
    bool remove(const std::filesystem::path& target) override;

    // Mount the VFS at the given mount point
    bool mount(const std::filesystem::path& mount_point);

    // Unmount the VFS
    bool unmount();

    [[nodiscard]] bool is_mounted() const { return mounted_; }

    // Add a file to the VFS (source -> virtual path mapping)
    void add_file(const std::string& virtual_path, const std::string& source_path);

    // Remove a file from the VFS
    void remove_file(const std::string& virtual_path);

    // Check if FUSE is available on this system
    [[nodiscard]] static bool is_available();

private:
    bool mounted_ = false;
    std::filesystem::path mount_point_;
    std::unordered_map<std::string, std::string> file_map_;  // virtual_path -> source_path
    mutable std::mutex mutex_;
};

}  // namespace engine
