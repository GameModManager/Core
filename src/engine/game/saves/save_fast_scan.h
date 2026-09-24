#pragma once

// Workspace-69xt: cheap scan-time parse for TESV-family saves.
//
// The Saves scan used to invoke the FULL per-save parser for every file:
// whole-file read + FULL zlib inflate of the data region + screenshot
// malloc/memcpy through the ABI - then the worker DISCARDED the screenshot.
// Measured ~10-33ms/save on tmpfs (far worse on HDD, where whole-file reads
// dominate: the user report is 103 saves / 24s = ~233ms/save); MO2 parses
// only header bytes per file during its scan, hence "literally instant".
//
// This reader parses header fields + plugin lists ONLY, skipping the
// screenshot bytes unread and capping the type-1 inflate to the region head
// (plugin lists sit at offset 0; the Papyrus/change-flag tail is never
// inflated). Field-identical to the full Gamebryo parse for everything it
// fills. The screenshot stays empty with has_heavy_data=false, so the first
// hover/selection re-parses the full save on demand (SavesTab::
// ensure_heavy_data) - the full parse moved behind the lazy boundary instead
// of running eagerly per file.
//
// Selection is knowledge-driven, never sniffed: the game plugin declares the
// "save_fast_format" hook with value kSaveFastFormatGamebryoTesv (new
// knowledge key, same shape as "save_extensions"), and the scan worker uses
// this reader only on a hook match. No per-game branches anywhere.

#include <cstdint>
#include <filesystem>
#include <string>

#include "engine/game/saves/save_game.h"
#include "engine/game/saves/save_reader.h"

namespace engine {

// Knowledge value for the "save_fast_format" hook selecting the reader
// below. Declared by the game plugin (Skyrim-family declares
// "gamebryo-tesv"); unknown values fall back to the full parser.
inline constexpr const char *kSaveFastFormatGamebryoTesv = "gamebryo-tesv";

// Cap on type-1 decompressed bytes during a fast scan. The plugin lists sit
// at region offset 0; 256KiB holds ~8000 plugin names (real saves carry at
// most hundreds). A file whose lists genuinely exceed the cap throws
// SaveNeedFullParse so the full parser serves it instead of dropping it.
inline constexpr std::uint64_t kFastScanDecompressedCap = 256 * 1024;

// Initial read window for a fast scan. Covers the header (~1KiB) + the
// largest real screenshot (SE 320x192x4 = 240KiB) + the chunk header + the
// first streams (~700KiB, where the plugin lists live), so a fast scan
// reads ~1MiB per file instead of the whole 10-25MiB save. A file whose
// head genuinely exceeds the window is retried with the full buffer; a
// corrupt file fails both and skips.
inline constexpr std::uint64_t kFastScanPrefixBytes = 1024 * 1024;

// Thrown when the fast reader cannot serve a file the FULL parser might
// still accept (currently: plugin data past the decompressed cap).
// Subclasses SaveParseError so any outer skip-on-error path still skips;
// the scan worker catches it first and reruns that file through the full
// parser instead of dropping the row.
class SaveNeedFullParse : public SaveParseError {
public:
  using SaveParseError::SaveParseError;
};

// Header + plugin lists for one TESV-family save ("TESV_SAVEGAME" magic).
// No screenshot (empty, has_heavy_data=false), no overlay/all_files (hover
// re-parse fills those). Throws SaveParseError on malformed input (the scan
// skips the file, MO2 listSaves parity) or SaveNeedFullParse when only the
// full parser can serve the file.
[[nodiscard]] SaveGame parse_gamebryo_tesv_fast(const std::filesystem::path &path,
                                                const std::string &game_id);

}  // namespace engine
