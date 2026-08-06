#pragma once

#include "engine/fs_utils.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace engine {

// Deploy progress: files_done filesystem operations completed out of
// files_total (link/unlink of the staged tree). Invoked from the executor's
// worker threads; callers that cross a thread boundary must forward it via a
// queued signal. total == 0 means there was nothing to do.
using DeployProgressFn = std::function<void(int files_done, int files_total)>;

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

// True when a mod file must be a REAL file (copied) in the staging tree rather
// than a symlink. Executables and scripts resolve sibling files relative to
// their own location; a symlinked lowerdir inode resolves through to the mod
// folder (/proc/self/exe, Wine path canonicalization), so skse64_loader.exe
// would look for SkyrimSE.exe in the mod folder and fail. Detection is
// deliberate: only things that RUN from within the merged view are copied -
// .exe (PE via Proton/Wine), .elf, .sh, and extensionless files that are
// either ELF binaries or #! scripts. Everything else (meshes, textures, DDS,
// .bin blobs, plugins) stays symlinked - it is only ever read, never run.
[[nodiscard]] bool is_executable_binary(const std::filesystem::path& path);

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

// Parallel variant of deploy_all_enabled_mods (PLAN §13.3, P8.4): the same
// contract, but the per-mod tree walks and the per-file link/unlink operations
// are farmed across a thread pool — link/unlink is IO+syscall bound and
// embarrassingly parallel across independent paths, so the first-ever full
// deploy of a large modlist scales with the available cores.
//
// Determinism: a contested target (two enabled mods shipping the same relative
// path) is won by the LAST mod in lexicographic folder order, replacing the
// directory_iterator order the sequential version depended on (which is
// arbitrary filesystem order, so the winner was never well-defined).
//
// Incremental O(Δ) redeploys: the executor persists a ledger of what's staged
// (target -> source) as <staging_dir>/.gmm_deploy_ledger. On a re-run, entries
// whose winner and staged file are unchanged cost a single stat and are
// skipped; only new/re-pointed/missing files are touched, and entries that
// stopped being winners (disabled/removed mod) are unlinked so a disabled
// mod's files can't linger in the overlay. The ledger lives inside staging, so
// the session-end wipe that clears .gmm_staging clears it too — the next
// launch is a full (parallel) deploy by design.
//
// num_threads: 0 (default) = std::thread::hardware_concurrency(), capped at
// 16. progress (if set) is invoked with (done, total) as link operations
// complete; total==0 means nothing to do.
[[nodiscard]] bool deploy_all_enabled_mods_parallel(
    const path& mods_dir,
    const path& staging_dir,
    const std::string& deploy_prefix,
    bool deploy_include_mod_id,
    const std::string& disable_mechanism,
    bool case_sensitive = true,
    unsigned int num_threads = 0,
    const DeployProgressFn& progress = {});

// Create lowercase symlink aliases inside a freshly deployed staging tree so a
// Windows (case-insensitive) game's path lookups resolve on the case-sensitive
// overlay. For every directory whose on-disk name has uppercase letters, an
// alias symlink <lowercase(name)> -> <name> is created in the same parent
// (e.g. Data/Interface gains Data/interface; the staging root's Data gains
// data). A game's lowercase spellings (Modex's relative "data/interface/
// modex/...", OAR's "data/meshes/...") then resolve through the alias chain
// into the canonical-case staged files, and its runtime writes funnel into one
// tree instead of spawning duplicate-case directories in the overwrite layer.
//
// Idempotent and non-destructive: a real entry at the alias name is left alone
// (it is already the canonical spelling); a stale generated alias is replaced.
// Runs on the caller's thread after the (possibly parallel) link phase.
// Returns the number of aliases created.
[[nodiscard]] std::size_t add_case_insensitive_aliases(
    const std::filesystem::path& staging_dir);

}

