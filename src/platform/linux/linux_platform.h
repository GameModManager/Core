#pragma once

#include "platform/platform_interface.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace engine {

// Linux platform module — XDG paths, Proton/Wine discovery, FUSE capabilities.
class LinuxPlatform : public PlatformInterface {
public:
    [[nodiscard]] std::string platform_name() const override { return "linux"; }

    [[nodiscard]] std::filesystem::path data_dir() const override;
    [[nodiscard]] std::filesystem::path config_dir() const override;
    [[nodiscard]] std::filesystem::path cache_dir() const override;

    [[nodiscard]] std::filesystem::path find_steam_root() const override;
    [[nodiscard]] std::filesystem::path find_proton() const override;
    [[nodiscard]] std::filesystem::path find_wine() const override;

    [[nodiscard]] bool launch_executable(
        const std::filesystem::path& executable,
        const std::vector<std::string>& args = {}) const override;

    [[nodiscard]] bool is_elevated() const override;
    [[nodiscard]] bool symlinks_available() const override { return true; }
    [[nodiscard]] bool junctions_available() const override { return false; }

private:
    static std::filesystem::path resolve_env_dir(
        const char* env_var, const std::filesystem::path& fallback);
};

}  // namespace engine
