#include "engine/core/log/crash_handler.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

// POSIX-only system headers. These must be included at global scope so the
// C library symbols (open/write/close, signal, backtrace, ...) land in the
// global namespace, not inside `namespace engine`.
#ifndef _WIN32
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include "platform/platform_interface.h"

namespace engine {

std::string CrashHandler::dump_dir_;

// ---------------------------------------------------------------------------
// Default dump directory
//
// Mirrors PlatformInterface::cache_dir() / "crash_dumps" on every platform so
// crash dumps land next to the rest of the app's cached data. Falls back to
// safe_home_dir() when the platform-specific environment variable is unset.
// ---------------------------------------------------------------------------
std::string CrashHandler::default_dump_dir() {
#ifdef _WIN32
    std::filesystem::path base;
    if (const wchar_t* la = _wgetenv(L"LOCALAPPDATA"); la && la[0] != L'\0') {
        base = la;
    } else {
        base = safe_home_dir() / L"AppData" / L"Local";
    }
    return (base / L"gamemodmanager" / L"cache" / L"crash_dumps").string();
#elif defined(__APPLE__)
    return (safe_home_dir() / "Library" / "Caches" / "GameModManager" /
            "crash_dumps")
        .string();
#else
    std::filesystem::path base;
    if (const char* xdg = std::getenv("XDG_CACHE_HOME");
        xdg && xdg[0] != '\0') {
        base = xdg;
    } else {
        base = safe_home_dir() / ".cache";
    }
    return (base / "GameModManager" / "crash_dumps").string();
#endif
}

// ---------------------------------------------------------------------------
// Windows: MiniDumpWriteDump via the unhandled-exception filter
// ---------------------------------------------------------------------------
#ifdef _WIN32

namespace {

std::filesystem::path g_dump_path;

LONG WINAPI windows_exception_handler(EXCEPTION_POINTERS* exception_info) {
    HANDLE hFile = CreateFileW(g_dump_path.wstring().c_str(), GENERIC_WRITE, 0,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{};
        info.ThreadId = GetCurrentThreadId();
        info.ExceptionPointers = exception_info;
        info.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                          MiniDumpNormal, &info, nullptr, nullptr);
        CloseHandle(hFile);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

std::filesystem::path make_dump_path(const std::string& dir) {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);
    char filename[128];
    std::snprintf(filename, sizeof(filename),
                  "%04d.%02d.%02d-%02d.%02d.%02d.dmp",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return std::filesystem::path(dir) / filename;
}

}  // namespace

void CrashHandler::install(const std::string& dump_dir) {
    dump_dir_ = dump_dir;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dump_dir), ec);
    g_dump_path = make_dump_path(dump_dir);
    SetUnhandledExceptionFilter(windows_exception_handler);
}

void CrashHandler::uninstall() {
    SetUnhandledExceptionFilter(nullptr);
}

// ---------------------------------------------------------------------------
// POSIX (Linux + macOS): signal handler + backtrace
// ---------------------------------------------------------------------------
#else

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

#if defined(__APPLE__)
void CrashHandler::macos_signal_handler(int sig, siginfo_t*, void*) {
    write_dump(sig);
    _exit(128 + sig);
}
#else
void CrashHandler::signal_handler(int sig) {
    write_dump(sig);
    _exit(128 + sig);
}
#endif

void CrashHandler::install(const std::string& dump_dir) {
    dump_dir_ = dump_dir;
    mkdirs(dump_dir_, 0755);

    struct sigaction sa{};
    sigemptyset(&sa.sa_mask);
#if defined(__APPLE__)
    sa.sa_sigaction = macos_signal_handler;
    sa.sa_flags = SA_SIGINFO;
#else
    sa.sa_handler = signal_handler;
    sa.sa_flags = SA_RESTART;
#endif
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

#endif  // _WIN32

// Free-function entry point used by main().
void install_crash_handler() {
    CrashHandler::install(CrashHandler::default_dump_dir());
}

}  // namespace engine
