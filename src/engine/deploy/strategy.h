#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace engine {

class DeploymentStrategy {
public:
    virtual ~DeploymentStrategy() = default;
    virtual bool deploy(const std::filesystem::path& source,
                        const std::filesystem::path& target) = 0;
    virtual bool remove(const std::filesystem::path& target) = 0;
};

class SymlinkStrategy : public DeploymentStrategy {
public:
    // case_sensitive=false routes targets through resolve_deploy_target_ci for
    // games whose filesystem is case-insensitive (Windows games), so CI-equal
    // directory paths merge into one on-disk casing.
    explicit SymlinkStrategy(bool case_sensitive = true);

    bool deploy(const std::filesystem::path& source,
                const std::filesystem::path& target) override;
    bool remove(const std::filesystem::path& target) override;

private:
    bool case_sensitive_;
};

// OverlayFS deploy strategy: mod files are symlinked into a staging directory
// (not game_dir). At launch time, the staging directory is layered on top of
// game_dir via OverlayFS, capturing all writes to the overwrite directory.
// game_dir is NEVER touched - no symlinks, no writes.
class OverlayFsDeployStrategy : public DeploymentStrategy {
public:
    explicit OverlayFsDeployStrategy(std::filesystem::path staging_dir,
                                     bool case_sensitive = true);

    bool deploy(const std::filesystem::path& source,
                const std::filesystem::path& target) override;
    bool remove(const std::filesystem::path& target) override;

    void set_mod_paths(const std::vector<std::filesystem::path>& paths);
    const std::vector<std::filesystem::path>& mod_paths() const;
    const std::filesystem::path& staging_dir() const { return staging_dir_; }

private:
    std::filesystem::path staging_dir_;
    std::vector<std::filesystem::path> mod_paths_;
    bool case_sensitive_;
};

}  // namespace engine
