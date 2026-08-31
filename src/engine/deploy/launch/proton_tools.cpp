#include "engine/deploy/launch/proton_tools.h"

#include "platform/platform.h"
#include "runtime/runtime.h"

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>

#ifndef _WIN32  // POSIX-only - guarded for MSVC compatibility

#endif

namespace fs = std::filesystem;

namespace engine {

namespace {

fs::path find_in_path(const std::string& name) {
    auto* path_env = std::getenv("PATH");
    if (!path_env) return {};
    std::istringstream ss(path_env);
    std::string token;
    while (std::getline(ss, token, ':')) {
        auto candidate = fs::path(token) / name;
        if (fs::exists(candidate)) return candidate;
    }
    return {};
}

// Detach a child that execs `argv`. stdin from /dev/null, own session so it
// survives our exit. Returns the child PID or -1 on fork failure.
int64_t spawn_detached(std::vector<std::string>& args) {
    if (args.empty()) return -1;

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

#ifdef _WIN32
    std::string cmd;
    for (const auto& a : args) {
        if (!cmd.empty()) cmd += " ";
        cmd += "\"" + a + "\"";
    }
    cmd += " &";
    return std::system(cmd.c_str()) == 0 ? 0 : -1;
#else
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (freopen("/dev/null", "r", stdin)) {}
        execvp(argv[0], argv.data());
        _exit(1);
    }
    return pid > 0 ? static_cast<int64_t>(pid) : -1;
#endif
}

// WINEPREFIX for a game's prefix: <steam>/steamapps/compatdata/<appid>/pfx.
fs::path game_prefix(const ProtonToolRequest& request) {
    if (request.steam_appid == 0 || !request.platform) return {};
    auto compat = request.platform->resolve_proton_prefix(request.steam_appid);
    if (compat.empty()) return {};
    // Proton stores drive_c under `pfx`; resolve_proton_prefix returns the
    // compatdata dir itself.
    return compat / "pfx";
}

bool is_wine_builtin(const std::vector<std::string>& args) {
    return args.size() == 1 &&
           (args[0] == "winecfg" || args[0] == "regedit");
}

}  // namespace

fs::path resolve_proton_runner(const ProtonToolRequest& request) {
    if (!request.platform) return {};
    if (!request.runner_override.empty()) {
        auto named = request.platform->find_proton_named(request.runner_override);
        if (!named.empty()) return named;
    }
    return ProtonRuntime::find_proton_binary(request.platform, request.steam_appid,
                                             request.runner_override);
}

bool proton_tooling_available(const ProtonToolRequest& request) {
    if (!find_in_path("protontricks").empty()) return true;
    if (find_in_path("winetricks").empty()) return false;
    // winetricks needs a wine build; Proton counts, system wine counts.
    if (!resolve_proton_runner(request).empty()) return true;
    return !find_in_path("wine").empty();
}

int64_t run_proton_tool(const ProtonToolRequest& request,
                        const std::vector<std::string>& args) {
    // 1. protontricks (preferred): routes every verb to the correct wine /
    //    winetricks inside the game's own prefix. Needs a known appid.
    auto protontricks = find_in_path("protontricks");
    if (!protontricks.empty() && request.steam_appid != 0 && request.platform) {
        auto steam_root = request.platform->find_steam_root();
        if (!steam_root.empty()) {
            setenv("STEAM_DIR", steam_root.string().c_str(), 1);
        }
        // Only a display name maps to protontricks' PROTON_VERSION; an
        // absolute path doesn't.
        if (!request.runner_override.empty() &&
            request.runner_override.find('/') == std::string::npos) {
            setenv("PROTON_VERSION", request.runner_override.c_str(), 1);
        }

        std::vector<std::string> cmd = {
            protontricks.string(), "--no-term",
            std::to_string(request.steam_appid),
        };
        cmd.insert(cmd.end(), args.begin(), args.end());
        return spawn_detached(cmd);
    }

    // 2. No protontricks. Wine builtins (winecfg/regedit) can still run via
    //    the Proton runner's own wine (`proton runinprefix`).
    if (is_wine_builtin(args)) {
        auto proton = resolve_proton_runner(request);
        if (!proton.empty() && request.platform && request.steam_appid != 0) {
            ProtonRuntime::prepare_proton_environment(request.platform,
                                                      request.game_dir,
                                                      request.steam_appid);
            std::vector<std::string> cmd = {
                proton.string(), "runinprefix", args[0],
            };
            return spawn_detached(cmd);
        }
    }

    // 3. Plain winetricks against the prefix (system wine). Last resort, but
    //    beats doing nothing when protontricks is missing.
    auto winetricks = find_in_path("winetricks");
    if (!winetricks.empty()) {
        auto prefix = game_prefix(request);
        if (!prefix.empty()) {
            setenv("WINEPREFIX", prefix.string().c_str(), 1);
        }
        std::vector<std::string> cmd = {winetricks.string()};
        cmd.insert(cmd.end(), args.begin(), args.end());
        return spawn_detached(cmd);
    }

    return -1;
}

int64_t run_proton_exe(const ProtonToolRequest& request,
                       const std::filesystem::path& exe) {
    if (exe.empty() || !fs::exists(exe)) return -1;

    // 1. protontricks can launch any exe in the game's prefix.
    auto protontricks = find_in_path("protontricks");
    if (!protontricks.empty() && request.steam_appid != 0 && request.platform) {
        auto steam_root = request.platform->find_steam_root();
        if (!steam_root.empty()) {
            setenv("STEAM_DIR", steam_root.string().c_str(), 1);
        }
        if (!request.runner_override.empty() &&
            request.runner_override.find('/') == std::string::npos) {
            setenv("PROTON_VERSION", request.runner_override.c_str(), 1);
        }
        std::vector<std::string> cmd = {
            protontricks.string(), "--no-term",
            std::to_string(request.steam_appid), exe.string(),
        };
        return spawn_detached(cmd);
    }

    // 2. Proton runtime fallback (same path the game launch uses).
    auto proton = resolve_proton_runner(request);
    if (!proton.empty()) {
        if (request.platform && request.steam_appid != 0) {
            ProtonRuntime::prepare_proton_environment(request.platform,
                                                      request.game_dir,
                                                      request.steam_appid);
        }
        std::vector<std::string> cmd = {
            proton.string(), "run", exe.string(),
        };
        return spawn_detached(cmd);
    }

    // 3. System wine with the game's prefix.
    auto wine = find_in_path("wine");
    if (!wine.empty()) {
        auto prefix = game_prefix(request);
        if (!prefix.empty()) {
            setenv("WINEPREFIX", prefix.string().c_str(), 1);
        }
        std::vector<std::string> cmd = {wine.string(), exe.string()};
        return spawn_detached(cmd);
    }

    return -1;
}

}  // namespace engine


#endif  // !_WIN32
