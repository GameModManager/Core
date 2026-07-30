#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace engine {

struct LaunchParams {
    std::filesystem::path executable;
    std::filesystem::path game_dir;
    std::filesystem::path overwrite_dir;
    uint32_t steam_appid = 0;
    bool is_windows_exe = false;

    // OverlayFS mod layer directories (priority order, first = highest).
    // When non-empty, the OverlayFsLauncher layers these over game_dir as
    // additional read-only overlay lowerdirs, enabling mod deployment without
    // touching game_dir.  Empty = legacy behavior (no mod layers, write capture only).
    std::vector<std::filesystem::path> extra_lowerdirs;
};

struct LaunchResult {
    int64_t pid = -1;
    bool overlay_launched = false;
};

LaunchResult launch_game(const LaunchParams& params);

void capture_overwrite(const std::filesystem::path& game_dir,
                       const std::filesystem::path& overwrite_dir,
                       std::filesystem::file_time_type capture_time);

// -- Process group monitoring --------------------------------------------
// Returns true if any non-zombie process still exists in the given PGID.
bool is_process_group_alive(int64_t pgid);

// Blocks until the entire process group has exited.
void wait_for_process_group(int64_t pgid,
                            int poll_ms = 500);

// Walk /proc via PPID chains to find ALL descendants of a root PID.
// Catches processes that created new sessions via setsid().
std::vector<int64_t> get_process_descendants(int64_t root_pid);

}
