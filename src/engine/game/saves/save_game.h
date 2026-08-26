#pragma once

// P2.5 Saves subsystem — Qt-free save-game model.
//
// This is the engine-side analogue of MOBase::ISaveGame
// (REFERENCES/modorganizer-preview_nif-gman-plugins++/mo2-abi/2.5.3beta11/
// include/uibase/isavegame.h) + GamebryoSaveGame
// (REFERENCES/modorganizer-game_bethesda/src/gamebryo/gamebryosavegame.{h,cpp}).
// It is a plain data struct, not an interface: the UI reads it directly, the
// parsers (registered via SaveParserFeature) fill it, and the missing-assets resolver
// (save_missing_assets.{h,cpp}) consumes it.
//
// Layout notes (all little-endian, verified against real Skyrim SE saves on
// /mnt/SSD):
//   - Magic "TESV_SAVEGAME" (13 bytes: ...GAME), then per-game header fields.
//   - Strings are u16 byte-length + raw bytes (UTF-8). They are NOT UTF-16
//     despite the "wstring" name in UESP/MO2 — a real save's "Vanilla Vanny"
//     is 13 bytes of ASCII with no NUL interleaving (hexdump-verified). MO2's
//     FileWrapper::read<QString> reads the same length+bytes and decodes UTF-8.
//   - The 8-byte FILETIME at the end of the header encodes the GAME'S LOCAL
//     WALL CLOCK at save time as if it were UTC (verified: the embedded value
//     matches the save filename's embedded timestamp exactly). filetime_to_epoch
//     converts it to epoch seconds with NO timezone adjustment, so the result
//     IS the wall-clock time the game displayed when saving. This deliberately
//     deviates from MO2's SkyrimSESaveGame, which subtracts 2.16e11 (6h) — a
//     machine-specific hack that produces a wrong displayed time on any
//     non-UTC+2 machine. See implementation.md Log 2026-08-06.
//   - SE (version 12) writes width/height, a u16 compression type, then an RGBA
//     screenshot; LE writes no compression type and an RGB screenshot.
//   - The data region after the screenshot is compressed: type 0 = raw,
//     type 1 = a sequence of independent zlib streams at 16-byte-aligned file
//     offsets (read by streaming chunks), type 2 = one LZ4 block.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace engine {

// Epoch seconds for a save's creation time. The value is the game's local
// wall clock at save time (see header comment); format it directly, never
// apply a UTC/local conversion on top.
using SaveEpochSeconds = std::int64_t;

// One parsed save game. The "cheap" header fields are always filled; the
// decompression-requiring fields (plugins, screenshot) are filled when the
// data region is read (always by the default parsers).
struct SaveGame {
    std::filesystem::path file_path;
    std::string game_id;  // "skyrim" | "skyrimse" | "skyrimvr" — parser tag

    // Header fields (MO2 ISaveGame + GamebryoSaveGame simple getters).
    SaveEpochSeconds creation_time = 0;
    std::string pc_name;
    std::uint16_t pc_level = 0;
    std::string pc_location;
    std::uint32_t save_number = 0;

    // Data-region fields (decompressed on demand in MO2; always here).
    std::vector<std::string> plugins;
    std::vector<std::string> light_plugins;
    std::vector<std::string> medium_plugins;  // unused by Skyrim; kept for FO4-style games
    std::vector<std::uint8_t> screenshot;     // raw RGBA (SE) or RGB (LE)
    int screenshot_width = 0;
    int screenshot_height = 0;

    // MO2 GamebryoSaveGame::getName(): "%1, #%2, Level %3, %4".
    [[nodiscard]] std::string display_name() const;
    // MO2 getSaveGroupIdentifier(): groups saves per character.
    [[nodiscard]] const std::string& save_group_identifier() const { return pc_name; }
    // True when the script-extender co-save (same basename, .skse) exists next
    // to this save (MO2 GamebryoSaveGame::hasScriptExtenderFile).
    [[nodiscard]] bool has_script_extender_file() const;
};

// FILETIME → epoch seconds, treating the 100ns-since-1601 value as UTC. No
// adjustment: the game writes local wall time here (see header comment).
[[nodiscard]] SaveEpochSeconds filetime_to_epoch(std::uint64_t filetime_100ns);

}  // namespace engine
