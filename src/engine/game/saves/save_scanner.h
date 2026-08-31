#pragma once

// Qt-free save-file scanner. Port of MO2's GameGamebryo::listSaves
// (REFERENCES/modorganizer-game_bethesda/src/gamebryo/gamegamebryo.cpp):
// filter a directory by the game's save extensions, parse each file with the
// game's parser, and SKIP (never fail on) files that don't parse - the .skse
// script-extender co-saves sit in the same directory and share the extension
// filter, so the magic check is what separates real saves from co-saves.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "engine/game/saves/save_game.h"

namespace engine {

using SaveParseFn = std::function<SaveGame(const std::filesystem::path&)>;

// Lists + parses save files in `dir` whose extension is in `extensions`
// (case-insensitive, with or without a leading dot). Unparseable files are
// skipped. Returns saves sorted by creation time, newest first - MO2
// savestab.cpp sorts getCreationTime() desc before rendering, so the scanner
// owns that ordering.
[[nodiscard]] std::vector<SaveGame> scan_saves(
    const std::filesystem::path& dir, const std::vector<std::string>& extensions,
    const SaveParseFn& parse_fn);

}  // namespace engine
