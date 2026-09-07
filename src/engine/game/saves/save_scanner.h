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

// Per-file callback for scan_saves_streaming. Invoked on the calling thread
// (NOT from parallel::for_each worker threads) after each successful parse,
// so the caller can marshal results onto a UI thread one at a time.
// Skipped/errored files do not invoke the callback.
using SaveEntryCallback = std::function<void(SaveGame)>;

// Lists save files in `dir` whose extension is in `extensions`
// (case-insensitive, with or without a leading dot). Returns paths in
// directory-walk order (unsorted, fastest to enumerate).
[[nodiscard]] std::vector<std::filesystem::path> enumerate_save_paths(
    const std::filesystem::path& dir, const std::vector<std::string>& extensions);

// Lists + parses save files in `dir` whose extension is in `extensions`
// (case-insensitive, with or without a leading dot). Unparseable files are
// skipped. Returns saves sorted by creation time, newest first - MO2
// savestab.cpp sorts getCreationTime() desc before rendering, so the scanner
// owns that ordering.
[[nodiscard]] std::vector<SaveGame> scan_saves(
    const std::filesystem::path& dir, const std::vector<std::string>& extensions,
    const SaveParseFn& parse_fn);

// Streaming variant: enumerates `dir` the same way scan_saves does, parses
// each file in parallel (matches the 6kn7 perf path), then invokes
// `on_entry` on the calling thread once per successful parse. Order is
// completion order (NOT creation_time order) - callers that need sorted
// output must re-sort, or insert into a sorted container as each entry
// arrives. Used by the UI to render saves as they finish parsing instead of
// waiting for the full batch (Workspace-0owv).
//
// `on_entry` runs on the calling thread; emit UI signals from there.
void scan_saves_streaming(
    const std::filesystem::path& dir, const std::vector<std::string>& extensions,
    const SaveParseFn& parse_fn, const SaveEntryCallback& on_entry);

}  // namespace engine
