#pragma once

#include <signal.h>
#include <string>

namespace engine {

class CrashHandler {
public:
    // Install the platform-specific crash handler. `dump_dir` is the directory
    // crash dumps are written to (created if it does not exist).
    static void install(const std::string& dump_dir);
    static void uninstall();

    // Compute the default crash dump directory: Platform::cache_dir()
    // / "crash_dumps", falling back to safe_home_dir() when the relevant
    // environment variable is unset. Mirrors the per-platform cache_dir()
    // logic so dumps land next to the rest of the app's cached data.
    [[nodiscard]] static std::string default_dump_dir();

private:
    // POSIX signal handler. On macOS this is installed with SA_SIGINFO, so it
    // takes the extended (siginfo) signature; on Linux the simple one-arg form
    // is used.
#if defined(__APPLE__)
    static void macos_signal_handler(int sig, siginfo_t*, void*);
#else
    static void signal_handler(int sig);
#endif
    static void write_dump(int sig);
    static std::string dump_dir_;
};

// Cross-platform install entry point: installs the crash handler writing to the
// default crash dump directory. Call this early in main(), before any UI or
// engine setup, so crashes during startup are captured too.
void install_crash_handler();

}  // namespace engine
