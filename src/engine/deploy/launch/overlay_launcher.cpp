#include "engine/deploy/launch/overlay_launcher.h"

#include "engine/core/util/debug_env.h"
#include "engine/core/util/fs_utils.h"
#include "engine/core/log/logger.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <system_error>

#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <sys/xattr.h>

namespace engine {

bool OverlayFsLauncher::is_supported(const std::filesystem::path& upper_dir) {
    // 1. Kernel must support overlay filesystem
    std::ifstream fs("/proc/filesystems");
    if (!fs.is_open()) {
        Logger::instance().warn("Overlay: cannot open /proc/filesystems");
        return false;
    }
    std::string line;
    bool has_overlay = false;
    while (std::getline(fs, line)) {
        if (line.find("overlay") != std::string::npos) {
            has_overlay = true;
            break;
        }
    }
    if (!has_overlay) {
        Logger::instance().warn("Overlay: 'overlay' not found in /proc/filesystems");
        return false;
    }

    // 2. Kernel >= 5.11 for unprivileged overlay with userxattr
    std::ifstream ver("/proc/version");
    if (!ver.is_open()) {
        Logger::instance().warn("Overlay: cannot open /proc/version");
        return false;
    }
    std::getline(ver, line);
    ver.close();

    auto dot1 = line.find('.');
    if (dot1 == std::string::npos) {
        Logger::instance().warn("Overlay: can't parse kernel version");
        return false;
    }
    auto space_before = line.rfind(' ', dot1);
    if (space_before == std::string::npos) {
        Logger::instance().warn("Overlay: can't find version number start");
        return false;
    }
    int major = std::atoi(line.substr(space_before + 1, dot1 - space_before - 1).c_str());
    if (major < 5) {
        Logger::instance().warn("Overlay: kernel major " + std::to_string(major) + " < 5");
        return false;
    }

    if (major == 5) {
        auto dot2 = line.find('.', dot1 + 1);
        if (dot2 == std::string::npos) {
            Logger::instance().warn("Overlay: can't parse kernel minor version");
            return false;
        }
        int minor = std::atoi(line.substr(dot1 + 1, dot2 - dot1 - 1).c_str());
        if (minor < 11) {
            Logger::instance().warn("Overlay: kernel " + std::to_string(major) + "." + std::to_string(minor) + " < 5.11");
            return false;
        }
    }

    auto kernel_ver = line.substr(0, line.find(' ', space_before + 1));

    // 3. If upper_dir is given, probe that the filesystem supports user xattrs
    if (!upper_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(upper_dir, ec);
        auto probe = upper_dir / ".gmm_xattr_probe";
        {
            std::ofstream ofs(probe.string());
            ofs << "x";
        }
        int ret = setxattr(probe.string().c_str(), "user.gmm.test", "1", 1, 0);
        std::filesystem::remove(probe, ec);
        if (ret != 0) {
            Logger::instance().warn("Overlay: filesystem at " + upper_dir.string() +
                " does NOT support user xattr (errno=" + std::to_string(errno) +
                "). OverlayFS unavailable on this filesystem.");
            return false;
        }
    }

    Logger::instance().debug("OverlayFsLauncher: supported (kernel " + kernel_ver + ")");
    return true;
}

int64_t OverlayFsLauncher::launch(const std::filesystem::path& executable,
                                   const std::filesystem::path& game_dir,
                                   const std::filesystem::path& upper_dir,
                                   const std::vector<std::string>& args,
                                   const std::vector<std::filesystem::path>& extra_lowerdirs,
                                   const std::filesystem::path& bind_mount_source,
                                   const std::filesystem::path& bind_mount_target,
                                   const std::filesystem::path& cwd) {
    Logger::instance().debug("OverlayFsLauncher::launch() executable=" + executable.string() +
        " game_dir=" + game_dir.string() + " upper_dir=" + upper_dir.string());

    if (!args.empty() && !extra_lowerdirs.empty()) {
        std::string dirs_log;
        for (auto& d : extra_lowerdirs) {
            if (!dirs_log.empty()) dirs_log += ", ";
            dirs_log += d.string();
        }
        Logger::instance().debug("Overlay: " + std::to_string(extra_lowerdirs.size()) +
            " extra lowerdir(s): " + dirs_log);
    }

    // Merged-view-aware gate. The executable may be physical (native game
    // file, the Proton binary for .exe entries), or it may be a merged-view
    // path that only resolves once the overlay is mounted over game_dir (a
    // mod-provided native executable, e.g. a root-override loader or a
    // Data-scoped tool). The child mounts BEFORE execv, so a staging-reachable
    // path resolves there; the gate only rejects what the overlay can never
    // provide. Directories (a mod's bin/ folder) are rejected up front too -
    // execv'ing them would otherwise fail cryptically inside the child.
    const std::filesystem::path staging =
        extra_lowerdirs.empty() ? std::filesystem::path() : extra_lowerdirs.back();
    if (!merged_view_executable_reachable(game_dir, staging, executable)) {
        Logger::instance().error("Overlay: executable not reachable as a regular file "
            "in merged view: " + executable.string());
        return -1;
    }
    if (!std::filesystem::exists(game_dir)) {
        Logger::instance().error("Overlay: game_dir not found: " + game_dir.string());
        return -1;
    }

    std::error_code ec;
    std::filesystem::create_directories(upper_dir, ec);
    if (ec) {
        Logger::instance().error("Overlay: failed to create upper_dir " + upper_dir.string() +
            ": " + ec.message());
        return -1;
    }

    auto work_dir = upper_dir.parent_path() / ".gmm_overlay_work";
    std::filesystem::create_directories(work_dir, ec);
    auto mount_point = work_dir / "mount";
    std::filesystem::create_directories(mount_point, ec);
    auto overlay_work = work_dir / "overlay_work";
    std::filesystem::create_directories(overlay_work, ec);

    Logger::instance().debug("Overlay: work_dir=" + work_dir.string() +
        " mount_point=" + mount_point.string());

    // Ensure executable permission (same as NativeRuntime). Only applies to
    // physically present executables: merged-view-only paths (e.g. a deployed
    // tool) resolve through the overlay, and the deploy already sets the exec
    // bit on staged copies. fs::status() without an error_code would THROW for
    // a merged-only path, so probe with one.
    {
        std::error_code perm_ec;
        auto st = std::filesystem::status(executable, perm_ec);
        if (!perm_ec) {
            auto perms = st.permissions();
            if ((perms & std::filesystem::perms::owner_exec) == std::filesystem::perms::none) {
                std::filesystem::permissions(executable,
                    perms | std::filesystem::perms::owner_exec
                           | std::filesystem::perms::group_exec
                           | std::filesystem::perms::others_exec, ec);
            }
        }
    }

    // Collect paths + outer UID/GID into a struct for the clone child.
    // UID/GID MUST be captured before clone() - calling getuid() inside the
    // new user namespace returns 65534 (nobody) because no mapping exists yet.
    uid_t outer_uid = getuid();
    gid_t outer_gid = getgid();

    struct CloneArgs {
        const std::filesystem::path executable;
        const std::filesystem::path game_dir;
        const std::filesystem::path mount_point;
        const std::filesystem::path overlay_work;
        const std::filesystem::path upper_dir;
        std::vector<std::string> exec_args;
        std::vector<std::filesystem::path> extra_lowerdirs;
        std::filesystem::path bind_mount_source;
        std::filesystem::path bind_mount_target;
        std::filesystem::path cwd;  // empty = chdir(game_dir)
        int stderr_fd;
        uid_t outer_uid;
        gid_t outer_gid;
    };

    // Open the stderr log file in the PARENT's namespace (before clone), so
    // the child inherits the fd and can dup2 it regardless of mount namespace.
    // GMM-owned artifacts go into the instance cache, never into Overwrite.
    int stderr_fd = -1;
    if (!args.empty() && !gmm_debug_enabled()) {
        auto cache_dir = upper_dir.parent_path() / "cache";
        std::error_code log_ec;
        std::filesystem::create_directories(cache_dir, log_ec);
        auto log_path = cache_dir / ".gmm_overlay_stderr.log";
        stderr_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }

    CloneArgs ca{executable, game_dir, mount_point, overlay_work, upper_dir, args, extra_lowerdirs, bind_mount_source, bind_mount_target, cwd, stderr_fd, outer_uid, outer_gid};
    constexpr size_t STACK_SIZE = 16384;

    // Clone into new user + mount namespace.
    // With CLONE_NEWUSER the child starts with the parent's UID in the new
    // namespace, and IS the namespace owner, so it can write its own uid_map.
    // CLONE_VFORK blocks the parent until the child execs or exits - after
    // clone() returns the child's stack is no longer live, so we can free it
    // immediately (no waiting needed).
    auto* stack = new (std::nothrow) char[STACK_SIZE];
    if (!stack) {
        Logger::instance().error("Overlay: failed to allocate child stack");
        return -1;
    }

    pid_t pid = clone([](void* arg) -> int {
        auto* ca = static_cast<const CloneArgs*>(arg);

        // ---- In new user + mount namespace ----
        // Map only our real UID/GID into the namespace (single-line map, so we keep
        // ns-owner capabilities for mount() but present the real uid to children).
        // Presenting uid 0 would trip root checks in games' launchers (e.g. Steam's
        // bin_steam.sh refuses when `id -u == 0`), so map uid → same uid.
        int fd = open("/proc/self/setgroups", O_WRONLY);
        if (fd >= 0) {
            if (write(fd, "deny", 4) < 0) { close(fd); _exit(12); }
            close(fd);
        }

        char map[64];
        int len = snprintf(map, sizeof(map), "%u %u 1", ca->outer_uid, ca->outer_uid);
        fd = open("/proc/self/uid_map", O_WRONLY);
        if (fd < 0) _exit(6);
        if (write(fd, map, (size_t)len) < 0) { close(fd); _exit(13); }
        close(fd);

        len = snprintf(map, sizeof(map), "%u %u 1", ca->outer_gid, ca->outer_gid);
        fd = open("/proc/self/gid_map", O_WRONLY);
        if (fd < 0) _exit(7);
        if (write(fd, map, (size_t)len) < 0) { close(fd); _exit(14); }
        close(fd);

        // We retain full ns-owner capabilities - mount overlay
        if (mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
            const char* e = strerror(errno);
            (void)write(STDERR_FILENO, "Overlay: mount(MS_PRIVATE) failed: ", 36);
            (void)write(STDERR_FILENO, e, strlen(e));
            (void)write(STDERR_FILENO, "\n", 1);
            _exit(8);
        }

        std::string lowerdir_str;
        std::string orig_dir;

        if (!ca->extra_lowerdirs.empty()) {
            // Validate each extra_lowerdir before attempting overlay setup
            std::vector<std::string> valid;
            for (const auto& p : ca->extra_lowerdirs) {
                auto s = p.string();
                if (access(s.c_str(), R_OK) == 0) {
                    valid.push_back(std::move(s));
                } else {
                    (void)write(STDERR_FILENO, "Overlay: warning: skipping extra_lowerdir ", 42);
                    (void)write(STDERR_FILENO, s.c_str(), s.size());
                    (void)write(STDERR_FILENO, "\n", 1);
                }
            }

            if (!valid.empty()) {
                // Preserve original game_dir by bind-mounting it to a temp location
                char tmpl[] = "/tmp/gmm_orig_XXXXXX";
                if (!mkdtemp(tmpl)) _exit(15);
                orig_dir = tmpl;
                if (mount(ca->game_dir.c_str(), orig_dir.c_str(), "", MS_BIND, NULL) != 0) {
                    const char* e = strerror(errno);
                    (void)write(STDERR_FILENO, "Overlay: mount(bind orig game_dir) failed: ", 44);
                    (void)write(STDERR_FILENO, e, strlen(e));
                    (void)write(STDERR_FILENO, "\n", 1);
                    _exit(16);
                }

                // Build lowerdir: valid extra layers first, then original game_dir as bottom
                for (size_t i = 0; i < valid.size(); ++i) {
                    if (i > 0) lowerdir_str += ":";
                    lowerdir_str += valid[i];
                }
                lowerdir_str += ":" + orig_dir;
            } else {
                lowerdir_str = ca->game_dir.string();
            }
        } else {
            lowerdir_str = ca->game_dir.string();
        }

        std::string data = "lowerdir=" + lowerdir_str
                         + ",upperdir=" + ca->upper_dir.string()
                         + ",workdir=" + ca->overlay_work.string()
                         + ",userxattr";

        if (mount("overlay", ca->mount_point.c_str(), "overlay", 0, data.c_str()) != 0) {
            const char* e = strerror(errno);
            (void)write(STDERR_FILENO, "Overlay: mount(overlay) failed, data=", 38);
            (void)write(STDERR_FILENO, data.c_str(), data.size());
            (void)write(STDERR_FILENO, " errno=", 7);
            (void)write(STDERR_FILENO, e, strlen(e));
            (void)write(STDERR_FILENO, "\n", 1);
            _exit(9);
        }

        if (mount(ca->mount_point.c_str(), ca->game_dir.c_str(), "", MS_BIND, NULL) != 0) {
            const char* e = strerror(errno);
            (void)write(STDERR_FILENO, "Overlay: mount(bind overlay mount over game_dir) failed: ", 58);
            (void)write(STDERR_FILENO, e, strlen(e));
            (void)write(STDERR_FILENO, "\n", 1);
            _exit(10);
        }

        // Optional extra bind mount (e.g. per-profile local saves): mount the
        // real source dir over the game-facing target dir so the game's writes
        // to the target land in the source. Best-effort - a missing source or a
        // mount failure logs to stderr but does not abort the launch (the game
        // runs with plain (non-local) saves in that case).
        if (!ca->bind_mount_source.empty() && !ca->bind_mount_target.empty()) {
            if (access(ca->bind_mount_source.c_str(), R_OK | W_OK) == 0) {
                if (mount(ca->bind_mount_source.c_str(), ca->bind_mount_target.c_str(),
                          "", MS_BIND, NULL) != 0) {
                    const char* e = strerror(errno);
                    (void)write(STDERR_FILENO, "Overlay: bind mount (local saves) failed: ", 42);
                    (void)write(STDERR_FILENO, e, strlen(e));
                    (void)write(STDERR_FILENO, "\n", 1);
                } else {
                    (void)write(STDERR_FILENO, "Overlay: bind mount (local saves) installed\n", 43);
                }
            } else {
                (void)write(STDERR_FILENO, "Overlay: bind mount (local saves) source missing, skipping\n", 60);
            }
        }

        setsid();

        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            if (!gmm_debug_enabled()) {
                if (ca->stderr_fd >= 0) {
                    // Use the fd opened by parent (survives namespace isolation)
                    dup2(ca->stderr_fd, STDERR_FILENO);
                    close(ca->stderr_fd);
                } else if (ca->exec_args.empty()) {
                    // Native binary, no stderr needed
                    dup2(devnull, STDERR_FILENO);
                }
            }
            close(devnull);
        }

        // Per-executable working directory (cwd, empty = game_dir). The
        // overlay mount is already in place at this point, so a cwd inside
        // the game dir resolves to the merged view.
        (void)chdir(ca->cwd.empty() ? ca->game_dir.c_str() : ca->cwd.c_str());

        if (!ca->exec_args.empty()) {
            std::vector<char*> argv;
            for (const auto& s : ca->exec_args)
                argv.push_back(const_cast<char*>(s.c_str()));
            argv.push_back(nullptr);
            execv(argv[0], argv.data());
        } else {
            char* argv[] = { const_cast<char*>(ca->executable.c_str()), nullptr };
            execv(argv[0], argv);
        }

        execl("/bin/sh", "sh", ca->executable.c_str(), nullptr);
        _exit(11);
    }, stack + STACK_SIZE,
       CLONE_VFORK | CLONE_NEWUSER | CLONE_NEWNS | SIGCHLD,
       &ca);

    if (pid < 0) {
        int saved_errno = errno;
        delete[] stack;
        Logger::instance().error("Overlay: clone() failed: " + std::string(strerror(saved_errno)));
        return -1;
    }

    // With CLONE_VFORK, parent resumes only after child has exec'd or _exit'd.
    // A single WNOHANG check distinguishes the two.
    int status;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid) {
        // Child exited during setup (before exec)
        delete[] stack;
        int code = -1;
        std::string reason;
        if (WIFEXITED(status)) {
            code = WEXITSTATUS(status);
            switch (code) {
                case 6:  reason = "open(/proc/self/uid_map) failed"; break;
                case 7:  reason = "open(/proc/self/gid_map) failed"; break;
                case 8:  reason = "mount(MS_PRIVATE) failed"; break;
                case 12: reason = "write(/proc/self/setgroups) failed"; break;
                case 13: reason = "write(/proc/self/uid_map) failed"; break;
                case 14: reason = "write(/proc/self/gid_map) failed"; break;
                case 9:  reason = "mount(overlay) failed"; break;
                case 10: reason = "mount(bind overlay over game_dir) failed"; break;
                case 11: reason = "execv + execl both failed"; break;
                case 15: reason = "mkdtemp(/tmp/gmm_orig_XXXXXX) failed"; break;
                case 16: reason = "mount(bind original game_dir to temp) failed"; break;
                default: reason = "unknown exit code"; break;
            }
        } else if (WIFSIGNALED(status)) {
            reason = "killed by signal " + std::to_string(WTERMSIG(status));
            code = -WTERMSIG(status);
        }
        Logger::instance().warn("Overlay: child exited during setup, code=" +
            std::to_string(code) + " (" + reason + ")");
        return -1;
    }

    // For wrapper/Proton launches (exec_args non-empty), poll for up to 2s
    // to catch processes that exec but die immediately - the single WNOHANG
    // above races past CLONE_VFORK while the child is still inside execv(),
    // so a process that fails within the first ~2 seconds looks like
    // "success" to the WNOHANG check.
    if (!args.empty()) {
        Logger::instance().debug("Overlay: child PID " + std::to_string(pid) +
            " exec'd (with args), entering 2s grace poll");
        for (int i = 0; i < 20; i++) {
            usleep(100000); // 100ms
            result = waitpid(pid, &status, WNOHANG);
            if (result == pid) {
                std::string reason;
                if (WIFEXITED(status)) {
                    int code = WEXITSTATUS(status);
                    reason = "exited with code " + std::to_string(code);
                    Logger::instance().warn("Overlay: child PID " +
                        std::to_string(pid) + " " + reason + " during grace window");
                } else if (WIFSIGNALED(status)) {
                    reason = "killed by signal " + std::to_string(WTERMSIG(status));
                    Logger::instance().warn("Overlay: child PID " +
                        std::to_string(pid) + " " + reason + " during grace window");
                } else {
                    reason = "unknown status";
                }
                // Log stderr capture location if it exists
                auto log_path = upper_dir.parent_path() / "cache" / ".gmm_overlay_stderr.log";
                std::error_code log_ec;
                auto log_size = std::filesystem::file_size(log_path, log_ec);
                if (!log_ec && log_size > 0) {
                    Logger::instance().warn("Overlay: stderr captured in " +
                        log_path.string() + " (" + std::to_string(log_size) + " bytes)");
                }
                delete[] stack;
                return -1;
            }
            if (result < 0) {
                Logger::instance().error("Overlay: waitpid error during grace poll: " +
                    std::string(strerror(errno)));
                delete[] stack;
                return -1;
            }
        }
        Logger::instance().debug("Overlay: child PID " + std::to_string(pid) +
            " survived 2s grace poll - truly running");
    } else {
        Logger::instance().debug("Overlay: child PID " + std::to_string(pid) +
            " exec'd (no args), no grace poll needed");
    }

    // Child is still running (exec'd successfully) - stack no longer in use
    delete[] stack;
    Logger::instance().debug("Overlay: child PID " + std::to_string(pid) + " running");
    return static_cast<int64_t>(pid);
}

bool OverlayFsLauncher::has_exited(int64_t pid) {
    if (pid <= 0) return true;
    int status;
    pid_t result = waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
    if (result == pid) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
        Logger::instance().debug("Overlay: child " + std::to_string(pid) +
            " exited with code " + std::to_string(code));
        return true;
    }
    if (result == 0) return false;
    return true;
}

}  // namespace engine
