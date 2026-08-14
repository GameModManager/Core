#include "runtime/runtime.h"

#include "platform/platform_interface.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#endif

namespace engine {

// --- NativeRuntime ---

bool NativeRuntime::launch(const std::filesystem::path& executable,
                           const std::filesystem::path& game_dir,
                           uint32_t /*steam_appid*/,
                           const std::vector<std::string>& args,
                           const std::filesystem::path& cwd) {
    if (!std::filesystem::exists(executable)) return false;

#ifdef _WIN32
    // On Windows, use ShellExecute to launch any registered file type
    std::string cmd = "\"" + executable.string() + "\"";
    for (const auto& a : args) cmd += " \"" + a + "\"";
    return std::system(cmd.c_str()) == 0;
#else
    // Ensure the file is executable
    auto st = std::filesystem::status(executable);
    auto perms = st.permissions();
    if ((perms & std::filesystem::perms::owner_exec) == std::filesystem::perms::none) {
        std::error_code ec;
        std::filesystem::permissions(executable,
            perms | std::filesystem::perms::owner_exec
                   | std::filesystem::perms::group_exec
                   | std::filesystem::perms::others_exec, ec);
    }

    pid_t pid = fork();
    if (pid == 0) {
        // Child process: detach from parent process group
        setsid();
        // Change to the configured working directory (game_dir when unset)
        // so relative paths in the executable work.
        const auto work_dir = cwd.empty() ? game_dir : cwd;
        if (!work_dir.empty())
            chdir(work_dir.c_str());
        // Redirect stdin from /dev/null so the child doesn't inherit our TTY
        if (freopen("/dev/null", "r", stdin)) {}
        // argv[0] = executable, then the configured args, then nullptr.
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        // If exec fails, try running through /bin/sh (for scripts without
        // proper shebang), preserving the args.
        std::vector<char*> sh_argv;
        sh_argv.push_back(const_cast<char*>("sh"));
        sh_argv.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& a : args)
            sh_argv.push_back(const_cast<char*>(a.c_str()));
        sh_argv.push_back(nullptr);
        execv("/bin/sh", sh_argv.data());
        _exit(1);
    } else if (pid > 0) {
        // Parent: don't wait - child is fully detached
        last_pid_ = static_cast<int64_t>(pid);
        return true;
    }
    return false;
#endif
}

bool NativeRuntime::is_available() const {
    return true;
}

// --- ProtonRuntime ---

ProtonRuntime::ProtonRuntime(const PlatformInterface* platform)
    : platform_(platform) {}

bool ProtonRuntime::launch(const std::filesystem::path& executable,
                           const std::filesystem::path& game_dir,
                           uint32_t steam_appid,
                           const std::vector<std::string>& args,
                           const std::filesystem::path& cwd) {
    if (!std::filesystem::exists(executable)) return false;
    if (!platform_) return false;

    auto proton = find_proton_binary(platform_, steam_appid, runner_override_);
    if (proton.empty()) return false;

    if (!prepare_proton_environment(platform_, game_dir, steam_appid)) return false;

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        const auto work_dir = cwd.empty() ? game_dir : cwd;
        if (!work_dir.empty())
            chdir(work_dir.c_str());
        if (freopen("/dev/null", "r", stdin)) {}
        // proton waitforexitandrun <exe> <args...>
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(proton.filename().c_str()));
        argv.push_back(const_cast<char*>("waitforexitandrun"));
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execv(proton.c_str(), argv.data());
        _exit(1);
    } else if (pid > 0) {
        last_pid_ = static_cast<int64_t>(pid);
        return true;
    }
    return false;
}

// --- Static helpers ---

std::filesystem::path ProtonRuntime::find_proton_binary(const PlatformInterface* platform,
                                                        uint32_t steam_appid,
                                                        const std::string& runner_override) {
    if (!platform) return {};

    if (!runner_override.empty()) {
        auto named = platform->find_proton_named(runner_override);
        if (!named.empty()) return named;
    }

    if (steam_appid > 0) {
        auto proton = platform->find_proton_for_game(steam_appid);
        if (!proton.empty()) return proton;
    }
    return platform->find_proton();
}

bool ProtonRuntime::prepare_proton_environment(const PlatformInterface* platform,
                                               const std::filesystem::path& game_dir,
                                               uint32_t steam_appid) {
    if (!platform) return false;

    auto steam_root = platform->find_steam_root();
    if (steam_root.empty()) return false;

    auto compat_data = platform->resolve_proton_prefix(steam_appid);
    if (compat_data.empty()) return false;

    setenv("STEAM_COMPAT_DATA_PATH", compat_data.string().c_str(), 1);
    setenv("STEAM_COMPAT_CLIENT_INSTALL_PATH", steam_root.string().c_str(), 1);
    setenv("STEAM_COMPAT_INSTALL_PATH", game_dir.string().c_str(), 1);
    setenv("STEAM_COMPAT_APP_ID", std::to_string(steam_appid).c_str(), 1);

    // Build library paths - all Steam library folders
    auto libs = platform->steam_library_paths();
    std::string library_paths;
    for (const auto& lib : libs) {
        if (!library_paths.empty()) library_paths += ":";
        library_paths += lib.string();
    }
    if (library_paths.empty()) {
        library_paths = steam_root.string();
    }
    setenv("STEAM_COMPAT_LIBRARY_PATHS", library_paths.c_str(), 1);

    return true;
}

bool ProtonRuntime::is_available() const {
    return platform_ && !find_proton_binary(platform_, 0, runner_override_).empty();
}

}  // namespace engine
