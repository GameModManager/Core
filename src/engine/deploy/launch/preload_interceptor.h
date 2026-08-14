#pragma once

#include <cstdint>
#include <filesystem>

namespace engine {

// Linux-specific: launches a process with LD_PRELOAD set to an intercept
// library that redirects file writes from game_dir to overwrite_dir.
// Works on any filesystem - no kernel features required beyond a standard
// Linux environment.  The intercept library is a small C .so that wraps
// open/openat/creat/rename/unlink/mkdir/rmdir.
//
// Trade-off vs OverlayFsLauncher:
//   + Works on NTFS, btrfs-coexist, NFS, etc.  No kernel version req.
//   + Every process exit, the target dir has the captured files, no waitpid.
//   - Only catches dynamically-linked binaries (over 99% of games).
//   - Adds ~ns per file syscall (strcmp + branch).
//   - Doesn't hide whiteouts or redirect deletes (intentional - game dir
//     stays clean and Overwrite only contains *new/modified* files).
class PreloadInterceptor {
public:
    // The intercept .so is always usable on Linux.  Returns true if the
    // shared library file exists on disk at our expected install path.
    static bool is_supported();

    // Launch executable with LD_PRELOAD set.  Returns child PID or -1.
    static int64_t launch(const std::filesystem::path& executable,
                          const std::filesystem::path& game_dir,
                          const std::filesystem::path& overwrite_dir);

    // Poll whether the process exited (non-blocking).  Returns true when
    // the PID is gone.
    static bool has_exited(int64_t pid);

    // Path where the intercept .so is expected at runtime.
    static std::filesystem::path so_path();
};

}  // namespace engine
