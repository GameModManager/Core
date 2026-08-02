#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace engine {

class PlatformInterface;

struct LaunchParams {
    std::filesystem::path executable;
    std::filesystem::path game_dir;
    std::filesystem::path overwrite_dir;
    uint32_t steam_appid = 0;
    bool is_windows_exe = false;

    // Platform services (Steam/Proton discovery, prefix resolution, user dirs).
    // All platform-specific path resolution in the launch path goes through this.
    // Null = platform services unavailable (discovery returns empty, no Proton).
    const PlatformInterface* platform = nullptr;

    // Transient per-session capture dir for "Output to mod" launches. When
    // non-empty, all game writes are captured here instead of overwrite_dir,
    // so the caller can relay the session's files into a specific mod folder
    // after the game exits. Empty = default Overwrite capture.
    std::filesystem::path output_capture_dir;

    // OverlayFS mod layer directories (priority order, first = highest).
    // When non-empty, the OverlayFsLauncher layers these over game_dir as
    // additional read-only overlay lowerdirs, enabling mod deployment without
    // touching game_dir.  Empty = legacy behavior (no mod layers, write capture only).
    std::vector<std::filesystem::path> extra_lowerdirs;
};

struct LaunchResult {
    int64_t pid = -1;
    bool overlay_launched = false;
    // Cgroup v2 path for reliable process tracking (empty = not available).
    // When non-empty, all game descendants are members of this cgroup.
    std::string cgroup_path;
};

LaunchResult launch_game(const LaunchParams& params);

void capture_overwrite(const std::filesystem::path& game_dir,
                       const std::filesystem::path& overwrite_dir,
                       std::filesystem::file_time_type capture_time);

// -- Cgroup v2 process tracking (primary) --------------------------------

struct CgroupHandle {
    std::string path;  // empty = not available / delegation failed
};

// Create a cgroup v2 directory under the user's delegated subtree.
// Returns empty handle when delegation isn't available (caller should
// fall back to subreaper + PPID walking).
CgroupHandle create_launch_cgroup(const std::string& name);

// Move a PID into the cgroup.  Once the root game process is in the
// cgroup, all its future descendants are automatically included.
void cgroup_add_pid(const CgroupHandle& h, int64_t pid);

// Read all PIDs currently in the cgroup (all game descendants).
std::vector<int64_t> cgroup_members(const CgroupHandle& h);

// Returns true when the cgroup contains zero processes.
bool cgroup_is_empty(const CgroupHandle& h);

// Kill every process in the cgroup (writes "1" to cgroup.kill).
// Significantly more reliable than kill(-pgid, SIGTERM).
void cgroup_kill(const CgroupHandle& h);

// -- Process group monitoring (fallback) ----------------------------------

// Returns true if any non-zombie process still exists in the given PGID.
bool is_process_group_alive(int64_t pgid);

// Blocks until the entire process group has exited.
void wait_for_process_group(int64_t pgid,
                            int poll_ms = 500);

// Walk /proc via PPID chains to find ALL descendants of a root PID.
// Catches processes that created new sessions via setsid().
// Used as fallback when cgroup delegation is unavailable.
std::vector<int64_t> get_process_descendants(int64_t root_pid);

}
