#pragma once

// Qt-free POSIX subprocess capture, shared by the engine modules that shell
// out to command-line tools (LOOT's gmm_lootcli, unrar for RAR archives whose
// dictionary exceeds libarchive's reader, ...). Single source of truth - never
// hand-roll a fork/exec/poll loop in a caller.

#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace engine {

// Platform-aware null device path for stdin redirection.
#ifdef _WIN32
constexpr auto DEV_NULL = "nul";
#else
constexpr auto DEV_NULL = "/dev/null";
#endif

struct CapturedProcess {
    bool ok = false;
    int exit_code = -1;
    std::string out;
    std::string err;
};

#ifndef _WIN32
// pipe2() equivalent: macOS lacks pipe2(), so create the pipe there and set
// FD_CLOEXEC on both ends via fcntl(). Linux/other POSIX use pipe2 directly.
inline int pipe_cloexec(int fds[2]) {
#ifdef GMM_PLATFORM_MACOS
    if (pipe(fds) != 0) return -1;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
#else
    return pipe2(fds, O_CLOEXEC);
#endif
}

// Run `args` (argv[0] resolved through PATH via execvp), capturing stdout and
// stderr fully, and wait for exit. stdin is redirected from /dev/null so a
// tool that would otherwise prompt interactively (e.g. unrar asking for a
// password) fails instead of hanging the caller. `ok` is false only if the
// process could not be started; exit_code carries the waitpid status otherwise.
inline CapturedProcess run_captured(const std::vector<std::string>& args) {
    CapturedProcess result;
    if (args.empty()) return result;

    int out_fds[2] = {-1, -1};
    int err_fds[2] = {-1, -1};
    if (pipe_cloexec(out_fds) != 0) return result;
    if (pipe_cloexec(err_fds) != 0) {
        close(out_fds[0]);
        close(out_fds[1]);
        return result;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(out_fds[0]);
        close(out_fds[1]);
        close(err_fds[0]);
        close(err_fds[1]);
        return result;
    }
    if (pid == 0) {
        close(out_fds[0]);
        close(err_fds[0]);
        dup2(out_fds[1], STDOUT_FILENO);
        dup2(err_fds[1], STDERR_FILENO);
        close(out_fds[1]);
        close(err_fds[1]);
        const int devnull = open(DEV_NULL, O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.data()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(out_fds[1]);
    close(err_fds[1]);

    char buf[8192];
    bool out_open = true;
    bool err_open = true;
    while (out_open || err_open) {
        struct pollfd fds[2];
        nfds_t n = 0;
        if (out_open) {
            fds[n].fd = out_fds[0];
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            ++n;
        }
        if (err_open) {
            fds[n].fd = err_fds[0];
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            ++n;
        }
        const int rc = poll(fds, n, 1000);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) continue;  // still running - a big extract can take a while
        for (nfds_t i = 0; i < n; ++i) {
            const bool is_out = fds[i].fd == out_fds[0];
            bool* open = is_out ? &out_open : &err_open;
            if ((fds[i].revents & (POLLIN | POLLHUP)) == 0) continue;
            const ssize_t got = read(fds[i].fd, buf, sizeof(buf));
            if (got > 0) {
                std::string* sink = is_out ? &result.out : &result.err;
                sink->append(buf, static_cast<size_t>(got));
            } else if (got == 0) {
                *open = false;
            } else if (errno != EINTR) {
                *open = false;
            }
        }
    }
    close(out_fds[0]);
    close(err_fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.ok = true;
    result.exit_code =
        WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    return result;
}
#else
// Windows fallback: no subprocess capture yet (the project is Linux-first;
// the tools that need this are Linux-side). Callers must treat `ok == false`.
inline CapturedProcess run_captured(const std::vector<std::string>&) {
    return {};
}
#endif

}  // namespace engine
