#pragma once

#include "platform/platform_interface.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace engine {

// macOS platform module - Library paths, Steam discovery, fork+exec launch.
class MacOSPlatform : public PlatformInterface {
public:
    [[nodiscard]] std::string platform_name() const override { return "macos"; }

    [[nodiscard]] std::filesystem::path data_dir() const override;
    [[nodiscard]] std::filesystem::path config_dir() const override;
    [[nodiscard]] std::filesystem::path cache_dir() const override;

    [[nodiscard]] std::filesystem::path find_steam_root() const override;
    [[nodiscard]] std::filesystem::path find_proton() const override { return {}; }

    [[nodiscard]] bool launch_executable(
        const std::filesystem::path& executable,
        const std::vector<std::string>& args = {}) const override;

    [[nodiscard]] bool is_elevated() const override;
    [[nodiscard]] bool symlinks_available() const override { return true; }
    [[nodiscard]] bool junctions_available() const override { return false; }
};

}  // namespace engine
