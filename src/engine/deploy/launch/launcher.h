#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace engine {

class Platform;

struct LaunchParams {
  std::filesystem::path executable;
  std::filesystem::path game_dir;
  std::filesystem::path overwrite_dir;
  uint32_t steam_appid = 0;
  bool is_windows_exe = false;

  // Platform services (Steam/Proton discovery, prefix resolution, user dirs).
  // All platform-specific path resolution in the launch path goes through this.
  // Null = platform services unavailable (discovery returns empty, no Proton).
  const Platform *platform = nullptr;

  // Transient per-session capture dir for "Output to mod" launches. When
  // non-empty, all game writes are captured here instead of overwrite_dir,
  // so the caller can relay the session's files into a specific mod folder
  // after the game exits. Empty = default Overwrite capture.
  std::filesystem::path output_capture_dir;

  // OverlayFS mod layer directories (priority order, first = highest).
  // When non-empty, the OverlayFsLauncher layers these over game_dir as
  // additional read-only overlay lowerdirs, enabling mod deployment without
  // touching game_dir.  Empty = legacy behavior (no mod layers, write capture
  // only).
  std::vector<std::filesystem::path> extra_lowerdirs;

  // True when the launch runs inside the OverlayFS sandbox (mount namespace
  // + overlay over game_dir + write capture into overwrite_dir). Set from the
  // instance's deploy_strategy: overlayfs games launch sandboxed, symlink
  // (default) games launch plain - mods are already linked into game_dir, so
  // no overlay is mounted and game writes go straight to game_dir.
  bool use_overlay = false;

  // Selected Proton runner (display name or absolute path to a `proton`
  // script). Empty = automatic (Steam per-game override, then latest).
  std::string proton_runner;

  // Optional extra bind mount inside the launch mount namespace (e.g. the
  // per-profile local-saves dir over the game-facing My Games __MO_Saves
  // folder). When both are non-empty, the OverlayFsLauncher mounts source
  // over target before exec so the game writes to dummy path land in the
  // profile dir. Empty = no extra bind.
  std::filesystem::path bind_mount_source;
  std::filesystem::path bind_mount_target;

  // === BROKEN FEATURE - DO NOT ENABLE ===
  // Historical arm switch for the libgmm_ci_intercept.so case-insensitive
  // interposer. The shim is broken - it shadows Wine's own case-insensitive
  // path handling and broke Pandora's game-tree reads (2026-08-09). do_launch
  // never preloads it unless GMM_ENABLE_BROKEN_CI_SHIM is explicitly set to a
  // truthy value. Kept only so the old wiring stays documented; do not build
  // on this flag.
  bool ci_resolve = false;

  // Per-executable environment overrides, each "KEY=VALUE". Applied to the
  // launched process (inherited by the overlay child / Proton / the game).
  // Entries without '=' are ignored with a warning. Empty = inherit the
  // parent environment unchanged.
  std::vector<std::string> environment;

  // Command-line arguments appended after the executable (Issue #34). The
  // exact argv the game sees is <executable> <args...>; for Windows
  // executables launched through Proton the same list is appended to
  // `proton waitforexitandrun <executable> <args...>`. Empty = no args.
  std::vector<std::string> args;

  // Working directory for the launched process. Empty = game_dir (the
  // pre-existing behavior). Relative paths are resolved against game_dir.
  // Applies to every launch path (overlay, LD_PRELOAD, native, Proton).
  std::filesystem::path cwd;
};

struct LaunchResult {
  int64_t pid = -1;
  bool overlay_launched = false;
  // Cgroup v2 path for reliable process tracking (empty = not available).
  // When non-empty, all game descendants are members of this cgroup.
  // Contains a std::string: never serialize LaunchResult across the
  // fork() result pipe as raw bytes - send only the POD fields (pid,
  // overlay_launched) and rebuild this from the parent's CgroupHandle.
  std::string cgroup_path;
};

// ProcessRunner wraps the game launch logic into a class interface.
// Construct with LaunchParams, call run() to execute the game process.
class ProcessRunner {
public:
  explicit ProcessRunner(LaunchParams params);

  // Execute the game process with the stored parameters.
  // Returns the result containing PID, overlay status, and cgroup path.
  LaunchResult run();

  // Access the stored parameters (read-only).
  const LaunchParams &params() const;

  // Access the last result (only valid after run() has been called).
  const LaunchResult &last_result() const;

private:
  LaunchParams params_;
  LaunchResult last_result_;
};

// Backward-compatible free function wrapper
LaunchResult launch_game(const LaunchParams &params);

void capture_overwrite(const std::filesystem::path &game_dir,
                       const std::filesystem::path &overwrite_dir,
                       std::filesystem::file_time_type capture_time,
                       bool case_insensitive = false);

// -- Cgroup v2 process tracking (primary) --------------------------------

struct CgroupHandle {
  std::string path; // empty = not available / delegation failed
};

// Create a cgroup v2 directory under the user's delegated subtree.
// Returns empty handle when delegation isn't available (caller should
// fall back to subreaper + PPID walking).
CgroupHandle create_launch_cgroup(const std::string &name);

// Move a PID into the cgroup.  Once the root game process is in the
// cgroup, all its future descendants are automatically included.
void cgroup_add_pid(const CgroupHandle &h, int64_t pid);

// Read all PIDs currently in the cgroup (all game descendants).
std::vector<int64_t> cgroup_members(const CgroupHandle &h);

// Returns true when the cgroup contains zero processes.
bool cgroup_is_empty(const CgroupHandle &h);

// Kill every process in the cgroup (writes "1" to cgroup.kill).
// Significantly more reliable than kill(-pgid, SIGTERM).
void cgroup_kill(const CgroupHandle &h);

// Remove a launch cgroup directory after the session ends.  Best-effort:
// a v2 cgroup can only be rmdir'd once empty, so this fails silently if
// processes still linger (or the path is already gone).
void cgroup_remove(const CgroupHandle &h);

// -- Process group monitoring (fallback) ----------------------------------

// Returns true if any non-zombie process still exists in the given PGID.
bool is_process_group_alive(int64_t pgid);

// Blocks until the entire process group has exited.
void wait_for_process_group(int64_t pgid, int poll_ms = 500);

// Walk /proc via PPID chains to find ALL descendants of a root PID.
// Catches processes that created new sessions via setsid().
// Used as fallback when cgroup delegation is unavailable.
std::vector<int64_t> get_process_descendants(int64_t root_pid);

} // namespace engine
