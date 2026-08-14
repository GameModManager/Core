#pragma once

#include "runtime/runtime.h"

#include <filesystem>
#include <string>

namespace engine {

// WineRuntime - launches Windows executables via Wine on Linux.
// Used for non-Steam, non-Proton cases where the user has Wine installed.
// ProtonRuntime already handles Steam-provided Proton; this handles standalone Wine.
class WineRuntime : public Runtime {
public:
    bool launch(const std::filesystem::path& executable,
                const std::filesystem::path& game_dir,
                uint32_t steam_appid = 0,
                const std::vector<std::string>& args = {},
                const std::filesystem::path& cwd = {}) override;
    bool is_available() const override;
    std::string name() const override { return "wine"; }

private:
    std::filesystem::path find_wine_binary() const;
};

}  // namespace engine
