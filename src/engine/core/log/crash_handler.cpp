#include "engine/core/log/crash_handler.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <execinfo.h>
#include <ucontext.h>

namespace engine {

std::string CrashHandler::dump_dir_;

static constexpr int kMaxFrames = 64;

// Recursively create directories (async-signal-safe subset: only uses mkdir + stat).
static bool mkdirs(const std::string& path, mode_t mode) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return true;

    // Find the parent
    size_t pos = path.find_last_of('/');
    if (pos != std::string::npos && pos > 0) {
        std::string parent = path.substr(0, pos);
        if (!parent.empty()) mkdirs(parent, mode);
    }

    return mkdir(path.c_str(), mode) == 0 || errno == EEXIST;
}

void CrashHandler::install(const std::string& dump_dir) {
    dump_dir_ = dump_dir;
    mkdirs(dump_dir_, 0755);

    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
}

void CrashHandler::uninstall() {
    signal(SIGSEGV, SIG_DFL);
    signal(SIGABRT, SIG_DFL);
    signal(SIGFPE,  SIG_DFL);
    signal(SIGBUS,  SIG_DFL);
    signal(SIGILL,  SIG_DFL);
}

void CrashHandler::signal_handler(int sig) {
    write_dump(sig);
    _exit(128 + sig);
}

void CrashHandler::write_dump(int sig) {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto time_t_now = system_clock::to_time_t(now);

    struct tm tm_buf{};
    localtime_r(&time_t_now, &tm_buf);

    char filename[128];
    std::snprintf(filename, sizeof(filename), "%s/%04d.%02d.%02d-%02d.%02d.%02d.dmp",
                  dump_dir_.c_str(),
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

    int fd = ::open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;

    auto write_str = [fd](const char* s) {
        [[maybe_unused]] auto _ = ::write(fd, s, std::strlen(s));
    };

    const char* sig_name = "UNKNOWN";
    switch (sig) {
        case SIGSEGV: sig_name = "SIGSEGV (segmentation fault)"; break;
        case SIGABRT: sig_name = "SIGABRT (abort)"; break;
        case SIGFPE:  sig_name = "SIGFPE (floating point exception)"; break;
        case SIGBUS:  sig_name = "SIGBUS (bus error)"; break;
        case SIGILL:  sig_name = "SIGILL (illegal instruction)"; break;
    }

    write_str("=== GameModManager Crash Dump ===\n");
    write_str("Signal: ");
    write_str(sig_name);
    write_str("\n");

    char ts_buf[64];
    std::snprintf(ts_buf, sizeof(ts_buf), "Time: %04d-%02d-%02d %02d:%02d:%02d\n",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    write_str(ts_buf);

    write_str("=== Stack Trace ===\n");

    void* frames[kMaxFrames];
    int count = backtrace(frames, kMaxFrames);
    char** symbols = backtrace_symbols(frames, count);

    if (symbols) {
        for (int i = 0; i < count; ++i) {
            write_str(symbols[i]);
            write_str("\n");
        }
        std::free(symbols);
    } else {
        write_str("(backtrace_symbols unavailable)\n");
    }

    write_str("=== End Dump ===\n");
    ::close(fd);
}

}  // namespace engine
