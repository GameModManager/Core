#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace engine {

using std::filesystem::path;

// Deploy all enabled (non-disabled) mods from instance_root/mods/ to staging_dir.
// staging_dir is created if it doesn't exist. Uses OverlayFsDeployStrategy internally
// to create symlinks under staging_dir/deploy_prefix/[mod_id/].
// disable_mechanism: the sentinel filename (e.g. ".disable") that marks a mod as disabled.
// Returns true if all mods deployed successfully (or nothing to deploy).
[[nodiscard]] bool deploy_all_enabled_mods(
    const path& mods_dir,
    const path& staging_dir,
    const std::string& deploy_prefix,
    bool deploy_include_mod_id,
    const std::string& disable_mechanism);

}

