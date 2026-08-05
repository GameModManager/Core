#include "engine/preload_interceptor.h"

#include "engine/debug_env.h"
#include "engine/log/logger.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace engine {

static std::filesystem::path find_so() {
    std::error_code ec;
    std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        Logger::instance().warn("Preload: cannot read /proc/self/exe: " + ec.message());
        return {};
    }
    auto dir = self.parent_path();

    auto candidate = dir / "libgmm_overlay_intercept.so";
    if (std::filesystem::exists(candidate, ec)) {
        Logger::instance().debug("Preload: found .so at " + candidate.string());
        return candidate;
    }

    candidate = dir.parent_path() / "lib" / "libgmm_overlay_intercept.so";
    if (std::filesystem::exists(candidate, ec)) {
        Logger::instance().debug("Preload: found .so at " + candidate.string());
        return candidate;
    }

    Logger::instance().warn("Preload: .so not found next to binary or in ../lib/");
    return {};
}

bool PreloadInterceptor::is_supported() {
    bool supported = !so_path().empty();
    Logger::instance().debug("PreloadInterceptor::is_supported() = " +
        std::string(supported ? "true" : "false"));
    return supported;
}

std::filesystem::path PreloadInterceptor::so_path() {
    static const auto cached = find_so();
    return cached;
}

int64_t PreloadInterceptor::launch(const std::filesystem::path& executable,
                                    const std::filesystem::path& game_dir,
                                    const std::filesystem::path& overwrite_dir) {
    Logger::instance().debug("PreloadInterceptor::launch() executable=" + executable.string() +
        " game_dir=" + game_dir.string() + " overwrite_dir=" + overwrite_dir.string());

    if (!is_supported()) {
        Logger::instance().error("Preload: not supported (.so not found)");
        return -1;
    }
    if (!std::filesystem::exists(executable)) {
        Logger::instance().error("Preload: executable not found: " + executable.string());
        return -1;
    }
    if (!std::filesystem::exists(game_dir)) {
        Logger::instance().error("Preload: game_dir not found: " + game_dir.string());
        return -1;
    }

    auto so = so_path();
    auto exe_str = executable.string();
    auto game_str = game_dir.string();
    auto overwrite_str = overwrite_dir.string();

    Logger::instance().debug("Preload: .so=" + so.string() + " LD_PRELOAD will be set");

    // Ensure executable permission (same as NativeRuntime)
    std::error_code ec;
    auto st = std::filesystem::status(executable, ec);
    if (!ec) {
        auto perms = st.permissions();
        if ((perms & std::filesystem::perms::owner_exec) == std::filesystem::perms::none) {
            std::filesystem::permissions(executable,
                perms | std::filesystem::perms::owner_exec
                       | std::filesystem::perms::group_exec
                       | std::filesystem::perms::others_exec, ec);
        }
    }

    pid_t pid = fork();
    if (pid == 0) {
        // ---- Child ----
        setsid();

        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            if (!gmm_debug_enabled())
                dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        if (chdir(game_dir.c_str()) != 0)
            _exit(12);

        // Set env vars for the intercept library
        if (setenv("GMM_GAME_DIR", game_str.c_str(), 1) != 0)
            _exit(13);
        if (setenv("GMM_OVERWRITE_DIR", overwrite_str.c_str(), 1) != 0)
            _exit(14);

        // Prepend our intercept .so to LD_PRELOAD
        auto cur_preload = getenv("LD_PRELOAD");
        std::string new_preload = so.string();
        if (cur_preload && cur_preload[0]) {
            new_preload += ":";
            new_preload += cur_preload;
        }
        if (setenv("LD_PRELOAD", new_preload.c_str(), 1) != 0)
            _exit(15);

        Logger::instance().debug("Preload child: LD_PRELOAD=" + new_preload);

        char* argv[] = { const_cast<char*>(exe_str.c_str()), nullptr };
        execv(argv[0], argv);

        // Fallback for scripts
        execl("/bin/sh", "sh", exe_str.c_str(), nullptr);
        _exit(5);
    }

    if (pid < 0) {
        Logger::instance().error("Preload: fork() failed: " + std::string(strerror(errno)));
        return -1;
    }

    // Check if child fails quickly (before or during exec)
    for (int retry = 0; retry < 20; retry++) {
        int status;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            int code = -1;
            std::string reason;
            if (WIFEXITED(status)) {
                code = WEXITSTATUS(status);
                switch (code) {
                    case 5:  reason = "execv + execl both failed"; break;
                    case 12: reason = "chdir(game_dir) failed"; break;
                    case 13: reason = "setenv(GMM_GAME_DIR) failed"; break;
                    case 14: reason = "setenv(GMM_OVERWRITE_DIR) failed"; break;
                    case 15: reason = "setenv(LD_PRELOAD) failed"; break;
                    default: reason = "exit code " + std::to_string(code); break;
                }
            } else if (WIFSIGNALED(status)) {
                code = -WTERMSIG(status);
                reason = "signal " + std::to_string(WTERMSIG(status));
            }
            Logger::instance().warn("Preload: child exited immediately, code=" +
                std::to_string(code) + " (" + reason + ")");
            return -1;
        }
        usleep(5000); // 5ms
    }

    Logger::instance().debug("Preload: child PID " + std::to_string(pid) + " running");
    return static_cast<int64_t>(pid);
}

bool PreloadInterceptor::has_exited(int64_t pid) {
    if (pid <= 0) return true;
    int status;
    pid_t result = waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
    if (result == pid) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
        Logger::instance().debug("Preload: child " + std::to_string(pid) +
            " exited with code " + std::to_string(code));
        return true;
    }
    if (result == 0) return false;
    return true;
}

}  // namespace engine
