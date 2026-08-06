#include "engine/launcher.h"

#include "engine/fs_utils.h"
#include "engine/log/logger.h"
#include "engine/overlay_launcher.h"
#include "engine/preload_interceptor.h"
#include "platform/platform_interface.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <signal.h>
#include <thread>
#include <unordered_set>
#include <sys/prctl.h>
#include <sys/wait.h>

namespace fs = std::filesystem;

namespace engine {

// Returns true when a live game chain (other than ourselves) shares our
// launch cgroup.  Used after the OverlayFS wrapper exits during the grace
// poll: Proton's `waitforexitandrun` hands a Steam game off and exits 0
// while the game's own processes keep running in the launch cgroup.
// Falling back would spawn a SECOND game instance.
static bool game_chain_alive_in_cgroup() {
    std::ifstream f("/proc/self/cgroup");
    std::string line, rel;
    while (std::getline(f, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto slash = line.find(':', colon + 1);
        if (slash == std::string::npos) continue;
        rel = line.substr(slash + 1);
        break;
    }
    // Only trust the check when the launch actually joined a GMM cgroup
    // (delegation was available).  Otherwise the ambient cgroup is shared
    // with unrelated processes and would produce a false positive.
    if (rel.find("/gmm-") == std::string::npos) return false;

    std::ifstream procs("/sys/fs/cgroup" + rel + "/cgroup.procs");
    int64_t pid;
    while (procs >> pid) {
        if (pid != getpid()) return true;
    }
    return false;
}

// Validates a launch executable against the merged view (physical, deployed
// into staging, or only reachable through the overlay mount the launcher is
// about to create) and logs a specific reason when it cannot be launched.
// Returns true when `exec_path` resolves to a regular file. The directory
// case (e.g. a mod's bin/ folder) is rejected with a clear message instead of
// a cryptic exec failure inside the overlay child.
static bool check_launch_executable(const fs::path& game_dir,
                                    const fs::path& staging,
                                    const fs::path& exec_path) {
    const fs::path resolved = merged_view_file_resolve(game_dir, staging, exec_path);
    if (resolved.empty()) {
        Logger::instance().error("Executable not found: " + exec_path.string());
        return false;
    }
    std::error_code rc;
    if (!fs::is_regular_file(resolved, rc)) {
        Logger::instance().error("Executable is not a regular file (resolved to " +
            resolved.string() + "): " + exec_path.string());
        return false;
    }
    return true;
}

static LaunchResult do_launch(const LaunchParams& params);

LaunchResult launch_game(const LaunchParams& params) {
    auto exec_path = params.executable;
    // Merged-view pre-check (same semantics as do_launch): the file may be
    // game-native (physical), deployed into .gmm_staging, or only visible
    // through the overlay mount that the launcher child is about to create.
    // A plain fs::exists runs in the caller's namespace where the overlay is
    // NOT mounted, so a staging-only mod executable (e.g. a root-override
    // mod's skse64_loader.exe) would look "missing" and abort the launch.
    const fs::path staging =
        params.extra_lowerdirs.empty() ? fs::path() : params.extra_lowerdirs.back();
    if (!check_launch_executable(params.game_dir, staging, exec_path)) {
        return {};
    }

#ifdef GMM_PLATFORM_LINUX
    // Create cgroup v2 scope for reliable process tracking.
    // When delegation is available, all game descendants land in this
    // cgroup automatically (fork inherits the parent's cgroup).
    CgroupHandle cgroup;
    {
        std::string cg_name = exec_path.stem().string() + "-" +
                              std::to_string(getpid());
        cgroup = create_launch_cgroup(cg_name);
    }

    // Fork a subreaper supervisor that claims PR_SET_CHILD_SUBREAPER so
    // orphaned grandchildren (wineserver, steam.exe, REPENTOGONLauncher,
    // etc.) reparent to it instead of PID 1.  This keeps the PPID chain
    // intact for get_process_descendants() and the process-tree display.
    int result_pipe[2];
    if (pipe(result_pipe) != 0) return {};

    pid_t supervisor = fork();
    if (supervisor < 0) {
        close(result_pipe[0]);
        close(result_pipe[1]);
        return {};
    }

    if (supervisor == 0) {
        // ---- subreaper ----
        close(result_pipe[0]);
        prctl(PR_SET_CHILD_SUBREAPER, 1);

        pid_t game = fork();
        if (game < 0) _exit(1);

        if (game == 0) {
            // ---- game-launch process ----
            // Join the cgroup BEFORE launching - all future children
            // (overlay clone, proton, steam.exe, etc.) inherit this
            // membership automatically.
            if (!cgroup.path.empty()) {
                std::ofstream(cgroup.path + "/cgroup.procs")
                    << getpid();
            }
            LaunchResult lr = do_launch(params);
            write(result_pipe[1], &lr, sizeof(lr));
            close(result_pipe[1]);
            _exit(0);
        }
        close(result_pipe[1]);

        // Reap-loop: stay alive until no children remain.
        // Because PR_SET_CHILD_SUBREAPER is set, any orphaned
        // grandchildren are reparented here and get reaped.
        // Each reaped child logs its exit status so a game that dies
        // moments after launch leaves a trail (crash signal vs clean
        // exit code) instead of a silent "Watchdog: cgroup empty".
        // raw_append() bypasses the logger mutex + callbacks: this process
        // was forked from a multithreaded Qt app, so taking the mutex here
        // could deadlock on a lock a vanished thread held at fork time.
        while (true) {
            int status;
            pid_t p = waitpid(-1, &status, 0);
            if (p < 0 && errno == ECHILD) break;
            std::string line;
            if (WIFEXITED(status)) {
                line = "[INF] [supervisor] reaped child " +
                       std::to_string(p) + " exited with code " +
                       std::to_string(WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                line = "[WRN] [supervisor] child " + std::to_string(p) +
                       " killed by signal " + std::to_string(WTERMSIG(status));
            } else {
                line = "[INF] [supervisor] reaped child " +
                       std::to_string(p) + " status " + std::to_string(status);
            }
            Logger::instance().raw_append(line + "\n");
        }
        _exit(0);
    }

    // ---- parent (the returned caller) ----
    close(result_pipe[1]);
    LaunchResult result;
    ssize_t n = read(result_pipe[0], &result, sizeof(result));
    close(result_pipe[0]);

    if (n != sizeof(result)) {
        waitpid(supervisor, nullptr, 0);
        return {};
    }

    result.pid = static_cast<int64_t>(supervisor);
    result.cgroup_path = cgroup.path;
    Logger::instance().debug(
        "launch_game spawned supervisor PID " + std::to_string(supervisor) +
        (cgroup.path.empty() ? "" : " (cgroup " + cgroup.path + ")"));
    return result;

#else
    // Non-Linux: launch directly, no subreaper or cgroup needed
    return do_launch(params);
#endif
}

// Core launch logic - extracted so it can run inside the subreaper child
// on Linux. Returns the actual game PID and whether overlay was used.
static LaunchResult do_launch(const LaunchParams& params) {
    Logger::instance().debug("Launching " + params.executable.string());

    auto exec_path = params.executable;
    const fs::path staging =
        params.extra_lowerdirs.empty() ? fs::path() : params.extra_lowerdirs.back();
    if (!check_launch_executable(params.game_dir, staging, exec_path)) {
        return {};
    }

    // Launch via the canonical (realpath) spelling so the game's own argv /
    // wine Z: path matches the overlay mountpoint.  The overlay is mounted at
    // the realpath of game_dir (mount() resolves symlinks), but the instance
    // path commonly goes through ~/.steam/steam -> ~/.local/share/Steam.  A
    // walk that crosses that symlink does not enter the namespace-local
    // overlay mount, so the game would silently see the pristine game dir
    // with no mods.  Realpath both so both spellings agree.
    std::error_code ec;
    auto canonical = fs::canonical(exec_path, ec);
    if (!ec) exec_path = canonical;

    int64_t pid = -1;

    // "Output to mod" sessions capture into a per-launch scratch dir instead
    // of the instance Overwrite folder. The xattr capability probe stays on
    // overwrite_dir (same filesystem as the scratch dir - both live under the
    // instance root).
    auto capture_dir = params.output_capture_dir.empty()
        ? params.overwrite_dir
        : params.output_capture_dir;

#ifdef GMM_PLATFORM_LINUX
    // Priority 1: OverlayFS - kernel VFS level, works for any binary format
    if (OverlayFsLauncher::is_supported(params.overwrite_dir)) {
        Logger::instance().debug("OverlayFS launcher: supported, trying overlay launch");

        if (params.is_windows_exe) {
            auto proton = ProtonRuntime::find_proton_binary(params.platform, params.steam_appid,
                                                            params.proton_runner);
            if (!proton.empty()) {
                ProtonRuntime::prepare_proton_environment(params.platform, params.game_dir, params.steam_appid);
                std::vector<std::string> ovl_args = {
                    proton.string(), "waitforexitandrun", exec_path.string()
                };
                pid = OverlayFsLauncher::launch(proton, params.game_dir,
                                                capture_dir, ovl_args,
                                                params.extra_lowerdirs);
            } else {
                Logger::instance().warn("OverlayFS: .exe but no Proton found, skipping");
            }
        } else {
            pid = OverlayFsLauncher::launch(exec_path, params.game_dir,
                                            capture_dir, {},
                                            params.extra_lowerdirs);
        }

        if (pid > 0) {
            Logger::instance().debug(
                "Launched inside OverlayFS overlay. All writes go to " +
                capture_dir.string());
            // The overlay child's stderr (wine/proton errors) is captured to
            // the instance cache when GMM_DEBUG is off - point at it now so a
            // crash AFTER the 2s grace poll still leaves a trace to read.
            if (params.is_windows_exe) {
                Logger::instance().debug(
                    "Game stderr will be captured at " +
                    (capture_dir.parent_path() / "cache" /
                     ".gmm_overlay_stderr.log").string());
            }
            return {pid, true};
        }

        // The Proton wrapper (`proton waitforexitandrun <game>`) can exit 0
        // during the grace poll after handing the game off to Steam, while the
        // game's own processes keep running in the launch cgroup. Falling back
        // would start a SECOND game instance (double-launch). If the game chain
        // is still alive in the cgroup, treat the overlay attempt as a success.
        if (game_chain_alive_in_cgroup()) {
            Logger::instance().debug(
                "OverlayFS wrapper exited but game processes remain in the launch "
                "cgroup - treating as launched (no fallback, avoiding double-launch)");
            return {getpid(), true};
        }
        Logger::instance().error("OverlayFS launcher returned failure, falling back");
    } else {
        Logger::instance().warn("OverlayFS not supported for this filesystem, skipping");
    }

    // Priority 2: LD_PRELOAD - intercepts libc calls (native binaries only)
    if (pid <= 0 && !params.is_windows_exe) {
        if (PreloadInterceptor::is_supported()) {
            Logger::instance().debug("PreloadInterceptor: trying LD_PRELOAD launch");
            pid = PreloadInterceptor::launch(exec_path, params.game_dir,
                                             capture_dir);
            if (pid > 0) {
                Logger::instance().debug(
                    "Launched with LD_PRELOAD intercept. Writes redirected to " +
                    capture_dir.string());
                return {pid, true};
            }
            Logger::instance().warn("PreloadInterceptor returned failure, falling back");
        } else {
            Logger::instance().warn("PreloadInterceptor: .so not found, skipping");
        }
    }
#else
    (void)pid;
#endif

    // Fallback: standard runtime launch + post-hoc capture
    {
        // A native executable that exists only in the merged view (deployed
        // mod file) can only be run by the OverlayFS launcher - neither the
        // LD_PRELOAD interceptor nor a plain fork/exec can reach it. Explain
        // that instead of a bare "not found" when overlay support failed.
        std::error_code phys_ec;
        if (!params.is_windows_exe && !fs::exists(exec_path, phys_ec) &&
            merged_view_file_exists(params.game_dir, staging, exec_path)) {
            Logger::instance().error(
                "Executable exists only in the merged view (deployed mod file); "
                "the OverlayFS launcher is required: " + exec_path.string());
        }

        std::unique_ptr<Runtime> runtime;
        if (params.is_windows_exe)
            runtime = std::make_unique<ProtonRuntime>(params.platform);
        if (!runtime)
            runtime = std::make_unique<NativeRuntime>();

        Logger::instance().debug("Launching: " + exec_path.string() +
            " (runtime: " + runtime->name() +
            ", appid: " + std::to_string(params.steam_appid) + ")");

        if (!runtime->launch(exec_path, params.game_dir, params.steam_appid)) {
            Logger::instance().error("Failed to launch game");
            return {};
        }
        pid = runtime->last_pid();
    }

    return {pid, false};
}

void capture_overwrite(const fs::path& game_dir,
                       const fs::path& overwrite_dir,
                       fs::file_time_type capture_time) {
    if (game_dir.empty() || overwrite_dir.empty()) return;

    try {
        std::error_code ec;
        if (!fs::exists(overwrite_dir))
            fs::create_directories(overwrite_dir, ec);

        std::vector<std::string> captured;

        // Walk game_dir and capture files written since capture_time
        {
            fs::recursive_directory_iterator it(
                game_dir, fs::directory_options::skip_permission_denied, ec);
            auto end = fs::recursive_directory_iterator();
            while (it != end && !ec) {
                auto& entry = *it;
                if (entry.is_regular_file() && !entry.is_symlink()) {
                    auto ft = entry.last_write_time(ec);
                    if (!ec && ft >= capture_time) {
                        auto ext = entry.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext != ".tmp" && ext != ".temp" && ext != ".bak") {
                            auto rel = fs::relative(entry.path(), game_dir, ec);
                            if (!ec) {
                                auto dest = overwrite_dir / rel;
                                fs::create_directories(dest.parent_path(), ec);
                                fs::copy_file(entry.path(), dest,
                                              fs::copy_options::overwrite_existing, ec);
                                if (!ec) {
                                    fs::remove(entry.path(), ec);
                                    captured.push_back(rel.string());
                                }
                            }
                        }
                    }
                    ec.clear();
                }
                it.increment(ec);
                if (ec) {
                    ec.clear();
                    it = fs::recursive_directory_iterator(
                        game_dir, fs::directory_options::skip_permission_denied, ec);
                }
            }
        }

        // Clean up empty directories
        bool any_removed = true;
        while (any_removed) {
            any_removed = false;
            fs::recursive_directory_iterator it(
                game_dir, fs::directory_options::skip_permission_denied, ec);
            auto end = fs::recursive_directory_iterator();
            while (it != end && !ec) {
                auto& entry = *it;
                if (entry.is_directory() && !entry.is_symlink()) {
                    auto dir = entry.path();
                    if (fs::is_empty(dir, ec) && !ec) {
                        fs::remove(dir, ec);
                        if (!ec) any_removed = true;
                    }
                    ec.clear();
                }
                it.increment(ec);
                if (ec) {
                    ec.clear();
                    it = fs::recursive_directory_iterator(
                        game_dir, fs::directory_options::skip_permission_denied, ec);
                }
            }
        }

        if (!captured.empty()) {
            Logger::instance().debug(
                "Overwrite capture: " + std::to_string(captured.size()) +
                " file(s) moved to Overwrite after process exit");
        }
    } catch (const std::exception& e) {
        Logger::instance().error("Overwrite capture failed: " + std::string(e.what()));
    }
}

bool is_process_group_alive(int64_t pgid) {
    if (pgid <= 0) return false;
    pid_t p = static_cast<pid_t>(pgid);

    int ret = kill(-p, 0);
    if (ret == 0) return true;
    if (errno == ESRCH) return false;
    if (errno == EPERM) return true;

    bool found = false;
    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) return false;

    struct dirent* entry;
    while ((entry = readdir(proc_dir)) && !found) {
        if (entry->d_type != DT_DIR) continue;
        pid_t pid = atol(entry->d_name);
        if (pid <= 0) continue;

        std::string spath = "/proc/" + std::to_string(pid) + "/stat";
        FILE* f = fopen(spath.c_str(), "r");
        if (!f) continue;
        char buf[4096] = {};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';

        char* cp = strrchr(buf, ')');
        if (!cp) continue;
        char* p = cp + 1;
        while (*p == ' ') ++p;
        if (*p == 'Z') continue;

        while (*p && *p != ' ') ++p;
        while (*p == ' ') ++p;
        while (*p && *p != ' ') ++p;
        while (*p == ' ') ++p;

        long pgrp = atol(p);
        found = (pgrp == static_cast<long>(pgid));
    }
    closedir(proc_dir);
    return found;
}

void wait_for_process_group(int64_t pgid, int poll_ms) {
    while (is_process_group_alive(pgid)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }
}

std::vector<int64_t> get_process_descendants(int64_t root_pid) {
    std::vector<int64_t> result;
    if (root_pid <= 0) return result;

    struct RawProc { pid_t pid; pid_t ppid; };
    std::vector<RawProc> all;

    DIR* dir = opendir("/proc");
    if (!dir) return result;
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        if (entry->d_type != DT_DIR) continue;
        pid_t pid = atol(entry->d_name);
        if (pid <= 0) continue;

        std::string spath = "/proc/" + std::to_string(pid) + "/stat";
        FILE* f = fopen(spath.c_str(), "r");
        if (!f) continue;
        char buf[4096] = {};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';

        char* cp = strrchr(buf, ')');
        if (!cp) continue;
        char* p = cp + 1;
        while (*p == ' ') ++p;
        while (*p && *p != ' ') ++p;
        while (*p == ' ') ++p;
        pid_t ppid = static_cast<pid_t>(atol(p));
        all.push_back({pid, ppid});
    }
    closedir(dir);

    std::unordered_set<pid_t> descendants;
    descendants.insert(static_cast<pid_t>(root_pid));
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& r : all) {
            if (descendants.count(r.pid)) continue;
            if (descendants.count(r.ppid)) {
                descendants.insert(r.pid);
                changed = true;
            }
        }
    }

    result.reserve(descendants.size());
    for (pid_t d : descendants)
        result.push_back(static_cast<int64_t>(d));
    return result;
}

// -- Cgroup v2 ------------------------------------------------------------

CgroupHandle create_launch_cgroup(const std::string& name) {
    CgroupHandle h;
    std::string uid = std::to_string(getuid());
    std::error_code ec;

    // Try user@.service/app.slice/ (systemd >= 244 delegates this subtree)
    fs::path base = "/sys/fs/cgroup/user.slice/user-" + uid
                  + ".slice/user@" + uid + ".service/app.slice";
    fs::create_directories(base, ec);
    ec = {};

    fs::path cg = base / ("gmm-" + name);
    if (fs::create_directory(cg, ec) || !ec) {
        h.path = cg.string();
        return h;
    }

    // Fallback: directly under user@.service/ (some distros omit app.slice)
    base = "/sys/fs/cgroup/user.slice/user-" + uid
         + ".slice/user@" + uid + ".service";
    cg = base / ("gmm-" + name);
    ec = {};
    if (fs::create_directory(cg, ec) || !ec) {
        h.path = cg.string();
        return h;
    }

    return h;
}

void cgroup_add_pid(const CgroupHandle& h, int64_t pid) {
    if (h.path.empty() || pid <= 0) return;
    std::ofstream f(h.path + "/cgroup.procs");
    if (f) f << pid;
}

std::vector<int64_t> cgroup_members(const CgroupHandle& h) {
    std::vector<int64_t> result;
    if (h.path.empty()) return result;
    std::ifstream f(h.path + "/cgroup.procs");
    int64_t pid;
    while (f >> pid) result.push_back(pid);
    return result;
}

bool cgroup_is_empty(const CgroupHandle& h) {
    if (h.path.empty()) return true;
    std::ifstream f(h.path + "/cgroup.procs");
    return f.peek() == std::ifstream::traits_type::eof();
}

void cgroup_kill(const CgroupHandle& h) {
    if (h.path.empty()) return;
    std::ofstream f(h.path + "/cgroup.kill");
    if (f) f << "1";
}

void cgroup_remove(const CgroupHandle& h) {
    if (h.path.empty()) return;
    // fs::remove (rmdir) on the cgroup directory itself - recursive delete
    // (remove_all) is wrong here: cgroup control files can't be unlinked
    // (EPERM), while rmdir on an empty v2 cgroup works.
    std::error_code ec;
    fs::remove(h.path, ec);
}

}  // namespace engine

