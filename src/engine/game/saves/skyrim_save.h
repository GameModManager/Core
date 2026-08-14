#pragma once

// Skyrim LE/SE/VR save parsers. Ports of MO2's SkyrimSaveGame and
// SkyrimSESaveGame (REFERENCES/modorganizer-game_bethesda/src/games/skyrim/ and
// .../skyrimse/). Both parse the same "TESV_SAVEGAME" header, differing in:
//   - LE: headerSize + version u32s are skipped (not read), saveNumber first;
//     SE reads them.
//   - SE: a u16 compression type + alpha channel are present when version == 12
//     (MO2: "SE has an additional uin16_t for compression, SE uses an alpha
//     channel, whereas LE does not").
//   - LE: the data region is uncompressed; the plugin list is formVersion u8,
//     pluginInfoSize u32, count u8, strings.
//   - SE: compressed (types 1/2); plugin list is formVersion u8, pluginInfoSize
//     u8, other u16, a pad u8, count u8, strings; when formVersion >= 78 a u16
//     light-plugin count + light-plugin strings follow.

#include <cstdint>
#include <filesystem>

#include "engine/game/saves/save_game.h"

namespace engine {

// Skyrim LE ("skyrim", GameSkyrim / SkyrimSaveGame).
[[nodiscard]] SaveGame parse_skyrim_save(const std::filesystem::path& path);

// Skyrim SE / VR ("skyrimse" / "skyrimvr", GameSkyrimSE / SkyrimSESaveGame,
// light plugins enabled). `game_id` tags the result.
[[nodiscard]] SaveGame parse_skyrimse_save(const std::filesystem::path& path,
                                           std::string game_id = "skyrimse");

}  // namespace engine
