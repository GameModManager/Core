#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "engine/game/saves/save_game.h"

namespace engine {

[[nodiscard]] SaveGame parse_skyrim_save(const std::filesystem::path& path);

[[nodiscard]] SaveGame parse_skyrimse_save(const std::filesystem::path& path,
                                           std::string game_id = "skyrimse");

}  // namespace engine
