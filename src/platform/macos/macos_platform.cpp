#ifndef __APPLE__
#error "This file should only be compiled on the correct platform"
#endif

#include "platform/macos/macos_platform.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace engine {

namespace {

std::filesystem::path home_dir() {
    auto home = std::getenv("HOME");
    if (!home) return {};
    return std::filesystem::path(home);
}

}  // namespace

// --- Library directory resolution ---

std::filesystem::path MacOSPlatform::data_dir() const {
    auto home = home_dir();
    if (home.empty()) return "/tmp/GameModManager";
    return home / "Library" / "Application Support" / "GameModManager";
}

std::filesystem::path MacOSPlatform::config_dir() const {
    auto home = home_dir();
    if (home.empty()) return "/tmp/GameModManager";
    return home / "Library" / "Application Support" / "GameModManager";
}

std::filesystem::path MacOSPlatform::cache_dir() const {
    auto home = home_dir();
    if (home.empty()) return "/tmp/GameModManager";
    return home / "Library" / "Caches" / "GameModManager";
}

// --- Steam discovery ---

std::filesystem::path MacOSPlatform::find_steam_root() const {
    auto home = home_dir();
    if (home.empty()) return {};

    auto root = home / "Library" / "Application Support" / "Steam";
    if (std::filesystem::exists(root)) {
        return root;
    }
    return {};
}

// --- Process launch ---

bool MacOSPlatform::launch_executable(
    const std::filesystem::path& executable,
    const std::vector<std::string>& args) const {
    if (!std::filesystem::exists(executable)) return false;

    // Double-fork so the game process is reparented to launchd and never
    // becomes a zombie of this long-lived GUI process.
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setsid();
        pid_t inner = fork();
        if (inner == 0) {
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(executable.c_str()));
            for (const auto& arg : args) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);
            execvp(executable.c_str(), argv.data());
            _exit(127);  // exec failed
        }
        _exit(inner < 0 ? 127 : 0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// --- Privilege check ---

bool MacOSPlatform::is_elevated() const {
    return geteuid() == 0;
}

// --- Home / temp / thread priority ---

std::filesystem::path MacOSPlatform::home_dir() const {
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0')
        return std::filesystem::path(home);
    return std::filesystem::temp_directory_path();
}

std::filesystem::path MacOSPlatform::temp_dir() const {
    return std::filesystem::temp_directory_path();
}

void MacOSPlatform::set_thread_low_priority() const {
    setpriority(PRIO_PROCESS, 0, 10);
}

}  // namespace engine
