#include "runtime/runtime.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace engine {

// --- NativeRuntime ---

bool NativeRuntime::launch(const std::filesystem::path& executable,
                           const std::string& /*game_id*/) {
    if (!std::filesystem::exists(executable)) return false;

    std::string cmd = "\"" + executable.string() + "\" &";
    return std::system(cmd.c_str()) == 0;
}

bool NativeRuntime::is_available() const {
    return true;
}

// --- ProtonRuntime ---

bool ProtonRuntime::launch(const std::filesystem::path& executable,
                           const std::string& /*game_id*/) {
    auto proton = find_proton();
    if (proton.empty()) return false;
    if (!std::filesystem::exists(executable)) return false;

    std::string cmd = "\"" + proton.string() + "\" \"" + executable.string() + "\" &";
    return std::system(cmd.c_str()) == 0;
}

bool ProtonRuntime::is_available() const {
    return !find_proton().empty();
}

std::filesystem::path ProtonRuntime::find_proton() const {
    auto home = std::getenv("HOME");
    if (!home) return {};

    std::filesystem::path steam_root = std::filesystem::path(home) / ".steam" / "steam";
    auto compat = steam_root / "steamapps" / "compatdata";

    if (!std::filesystem::exists(compat)) return {};

    // Look for a Proton installation in the Steam tools directory
    auto tools = steam_root / "steamapps" / "common";
    if (!std::filesystem::exists(tools)) return {};

    for (const auto& entry : std::filesystem::directory_iterator(tools)) {
        if (entry.is_directory()) {
            auto name = entry.path().filename().string();
            if (name.find("Proton") != std::string::npos) {
                auto proton_bin = entry.path() / "proton";
                if (std::filesystem::exists(proton_bin)) {
                    return proton_bin;
                }
            }
        }
    }

    return {};
}

}  // namespace engine
