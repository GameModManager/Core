#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace engine {

// Linux-specific: launches a process inside an OverlayFS mount so every file
// it writes lands directly in the upper directory (Overwrite) instead of the
// real game directory.  Uses unprivileged mount namespace + user namespace.
// Falls back to -1 if overlay isn't available - caller should handle the fallback.
class OverlayFsLauncher {
public:
    // Check whether unprivileged overlay mounts are supported.  When
    // upper_dir is non-empty, additionally probes that the filesystem
    // containing upper_dir supports user xattrs (required by overlay).
    static bool is_supported(const std::filesystem::path& upper_dir = {});

    // Launch executable (and optional args) inside an overlay where game_dir is
    // lower (r/o) and upper_dir captures all writes.  Returns child PID or -1 on
    // failure.  After the process exits, upper_dir will contain every file the
    // process wrote - no post-hoc scanning needed.  The mount namespace
    // disappears automatically when the last child process exits.
    //
    // When extra_lowerdirs is non-empty, these directories are layered on top of
    // game_dir as additional read-only overlay layers (first = highest priority).
    // This enables mod deployment without touching game_dir - deploy symlinks
    // into a staging dir and pass it as an extra lowerdir.
    //
    // bind_mount_source/bind_mount_target, when non-empty, add one additional
    // real directory bind mount inside the same mount namespace (e.g. the
    // per-profile local-saves dir mounted over the game-facing My Games
    // __MO_Saves folder), so the game sees one dir but writes hit another.
    // Both paths must exist on the host; a missing source is logged and skipped.
    static int64_t launch(const std::filesystem::path& executable,
                          const std::filesystem::path& game_dir,
                          const std::filesystem::path& upper_dir,
                          const std::vector<std::string>& args = {},
                          const std::vector<std::filesystem::path>& extra_lowerdirs = {},
                          const std::filesystem::path& bind_mount_source = {},
                          const std::filesystem::path& bind_mount_target = {},
                          const std::filesystem::path& cwd = {});

    // Poll whether a launch()-ed process has exited (non-blocking).
    // Returns true if the process is gone.
    static bool has_exited(int64_t pid);
};

}  // namespace engine
