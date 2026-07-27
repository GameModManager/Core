#include "runtime/wine_runtime.h"

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace engine {

std::filesystem::path WineRuntime::find_wine_binary() const {
    // 1. Check WINE env var
    auto* wine_env = std::getenv("WINE");
    if (wine_env && wine_env[0] != '\0') {
        auto path = std::filesystem::path(wine_env);
        if (std::filesystem::exists(path)) return path;
    }

    // 2. Search PATH for wine
    auto* path_env = std::getenv("PATH");
    if (path_env) {
        std::istringstream ss(path_env);
        std::string token;
        while (std::getline(ss, token, ':')) {
            auto wine_path = std::filesystem::path(token) / "wine";
            if (std::filesystem::exists(wine_path)) return wine_path;
        }
    }

    // 3. Common locations
    std::vector<std::filesystem::path> candidates = {
        "/usr/bin/wine",
        "/usr/local/bin/wine",
        "/opt/wine/bin/wine",
    };

    for (const auto& c : candidates) {
        if (std::filesystem::exists(c)) return c;
    }

    return {};
}

bool WineRuntime::launch(const std::filesystem::path& executable,
                         const std::filesystem::path& /*game_dir*/,
                         uint32_t /*steam_appid*/) {
    auto wine = find_wine_binary();
    if (wine.empty()) return false;
    if (!std::filesystem::exists(executable)) return false;

    std::string cmd = "\"" + wine.string() + "\" \"" + executable.string() + "\" &";
    return std::system(cmd.c_str()) == 0;
}

bool WineRuntime::is_available() const {
    return !find_wine_binary().empty();
}

}  // namespace engine
