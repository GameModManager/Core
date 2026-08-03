#pragma once

#include "engine/fs_utils.h"

#include <filesystem>
#include <string>
#include <vector>

namespace engine {

using std::filesystem::path;

// Resolve an absolute deploy target against the on-disk tree
// case-insensitively. Every existing directory component whose name differs
// only in case from the requested spelling is replaced by the on-disk casing,
// so a mod shipping both "Meshes" and "meshes" collapses into one directory -
// Windows games resolve paths case-insensitively, and the case-sensitive
// overlay mount must not split a mod across two dirs. Components that do not
// exist yet keep the requested spelling (the deploy creates them). The final
// component is not matched: CI-equal file names are a rare packaging bug and
// land side-by-side.
//
// Exact-cased exists() is the fast path; the parent-dir CI scan runs only when
// a component is absent at its requested casing, so well-formed mods cost one
// stat per directory component and nothing more.
//
// Inline so deploy consumers (strategy_symlink.cpp, strategy_overlayfs_deploy
// .cpp, deploy_utils.cpp) need no extra link dependencies.
[[nodiscard]] inline std::filesystem::path resolve_deploy_target_ci(
    const std::filesystem::path& target) {
    std::error_code ec;
    std::filesystem::path cur = target.root_path();
    if (cur.empty()) {
        // Not absolute (shouldn't happen for deploy targets): fall back to the
        // first component's parent so the walk still works.
        cur = target.begin()->parent_path();
    }
    std::vector<std::filesystem::path> comps;
    for (const auto& part : target.relative_path()) comps.push_back(part);

    // Match every directory component (all but the final file name).
    for (size_t i = 0; i + 1 < comps.size(); ++i) {
        const auto& comp = comps[i];
        const std::filesystem::path exact = cur / comp;
        if (std::filesystem::exists(exact, ec) || ec) {
            cur = exact;
            continue;
        }
        // Exact casing absent: reuse an existing CI-matching entry's casing.
        std::filesystem::path match;
        bool found = false;
        for (const auto& entry : std::filesystem::directory_iterator(cur, ec)) {
            if (ec) break;
            if (name_matches_ci(entry.path(), comp.string())) {
                match = entry.path();
                found = true;
                break;
            }
        }
        cur = found ? match : exact;
    }
    return cur / comps.back();
}

// Deploy all enabled (non-disabled) mods from instance_root/mods/ to staging_dir.
// staging_dir is created if it doesn't exist. Uses OverlayFsDeployStrategy internally
// to create symlinks under staging_dir/deploy_prefix/[mod_id/].
// disable_mechanism: the sentinel filename (e.g. ".disable") that marks a mod as disabled.
// case_sensitive: true (default) preserves each mod's on-disk casing in the
// staging tree; false routes every target through resolve_deploy_target_ci for
// games whose filesystem is case-insensitive (Windows games).
// Returns true if all mods deployed successfully (or nothing to deploy).
[[nodiscard]] bool deploy_all_enabled_mods(
    const path& mods_dir,
    const path& staging_dir,
    const std::string& deploy_prefix,
    bool deploy_include_mod_id,
    const std::string& disable_mechanism,
    bool case_sensitive = true);

}

