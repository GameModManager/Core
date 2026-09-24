// Workspace-69xt: cheap scan-time parse for TESV-family saves. Layout mirrors
// the Plugins Gamebryo packet field-for-field
// (Plugins/src/shared/Gamebryo/GamebryoSaveGame.cpp fetch_information_fields,
// SkyrimSESaveGame.cpp / SkyrimSaveGame.cpp fetch_data_fields, all MO2 ports):
//   header: skip u32 headerSize, u32 version, u32 saveNumber, wstring name,
//     u32 level (truncated to u16), wstring location, wstring timeOfDay
//     (discarded), wstring race (discarded), skip u16 gender + 2 floats,
//     u64 FILETIME -> filetime_to_epoch (GMM wall-clock semantics, same fn
//     the full parser uses).
//   screenshot: u32 w, u32 h, [u16 compressionType + RGBA iff version == 12,
//     else RGB] - SKIPPED unread (the scan never shows pixels).
//   data region: SE (version == 12): u8 saveGameVersion, u8 pluginInfoSize,
//     u16 other, skip 1 pad, u8 pluginCount + names, iff saveGameVersion >=
//     78: u16 lightCount + names. LE: skip 1 formVersion, skip u32
//     pluginInfoSize, u8 pluginCount + names, no light plugins.

#include "engine/game/saves/save_fast_scan.h"

namespace engine {

namespace {

  // The parse itself, parameterized by read window: `prefix` bytes buffered
  // (0 = whole file). Everything else is identical.
  SaveGame parse_with_prefix(const std::filesystem::path &path,
                             const std::string &game_id, std::uint64_t prefix) {
    SaveReader r(path, "TESV_SAVEGAME", prefix);
    SaveGame out;
    out.file_path = path;
    out.game_id   = game_id;

    r.skip(4);  // header size (does NOT bound the strings; MO2 ignores it)
    const std::uint32_t version = r.u32();
    out.save_number             = r.u32();
    out.pc_name                 = r.wstring();
    const std::uint32_t level   = r.u32();
    out.pc_level                = static_cast<std::uint16_t>(level);
    out.pc_location             = r.wstring();
    r.wstring();  // time of day (discarded)
    r.wstring();  // race (discarded)
    r.skip(2);    // gender
    r.skip(8);    // experience gathered/required (2 floats)
    out.creation_time = filetime_to_epoch(r.u64());

    const std::uint32_t w     = r.u32();
    const std::uint32_t h     = r.u32();
    const bool is_se          = (version == 12);
    std::uint16_t compression = 0;
    if (is_se) {
      compression = r.u16();
    }
    // Screenshot skip: w*h*pixel_size bytes lie between here and the data
    // region. Seek past them without reading (the old scan paid a 240KB
    // malloc+memcpy per save here, then threw the pixels away). Guard the
    // multiply-then-compare against a hostile w/h: a real screenshot is
    // always smaller than the buffered window (or the file on retry).
    const std::uint64_t pixels = static_cast<std::uint64_t>(w) * h * (is_se ? 4u : 3u);
    if (pixels > r.size()) {
      throw SaveParseError("screenshot size exceeds file for " + path.string());
    }
    r.skip(static_cast<std::size_t>(pixels));

    r.begin_compressed(compression, kFastScanDecompressedCap);
    if (is_se) {
      const std::uint8_t save_game_version = r.u8();
      r.u8();     // plugin info size
      r.u16();    // other (unknown)
      r.skip(1);  // pad byte before the plugin count
      const std::uint8_t plugin_count = r.u8();
      out.plugins.reserve(plugin_count);
      try {
        for (std::uint8_t i = 0; i < plugin_count; ++i) {
          out.plugins.push_back(r.wstring());
        }
        if (save_game_version >= 78) {
          const std::uint16_t light_count = r.u16();
          out.light_plugins.reserve(light_count);
          for (std::uint16_t i = 0; i < light_count; ++i) {
            out.light_plugins.push_back(r.wstring());
          }
        }
      } catch (const SaveParseError &) {
        // The capped inflate ran out mid-list: the plugin data genuinely
        // exceeds kFastScanDecompressedCap. The full parser (uncapped
        // inflate) can still serve this file - ask the worker to rerun it
        // there instead of dropping the row. (A truly corrupt file takes
        // the same path and the full parser rejects it, same outcome.)
        throw SaveNeedFullParse("plugin data past fast-scan cap for " + path.string());
      }
    } else {
      // LE: no light plugins, wider plugin-info-size field, no pad/other.
      r.skip(1);  // form version
      r.skip(4);  // plugin info size (u32 in LE)
      const std::uint8_t plugin_count = r.u8();
      out.plugins.reserve(plugin_count);
      try {
        for (std::uint8_t i = 0; i < plugin_count; ++i) {
          out.plugins.push_back(r.wstring());
        }
      } catch (const SaveParseError &) {
        throw SaveNeedFullParse("plugin data past fast-scan cap for " + path.string());
      }
    }

    // Heavy data deliberately absent: no screenshot pixels, so hover pays one
    // full parse on demand (SavesTab::ensure_heavy_data) instead of every
    // file paying it during the scan.
    out.has_heavy_data = false;
    return out;
  }

}  // namespace

SaveGame parse_gamebryo_tesv_fast(const std::filesystem::path &path,
                                  const std::string &game_id) {
  try {
    return parse_with_prefix(path, game_id, kFastScanPrefixBytes);
  } catch (const SaveNeedFullParse &) {
    throw;
  } catch (const SaveParseError &) {
    // The 1MiB window ran dry (screenshot or lists past the window, freak
    // file): retry with the whole file buffered. A genuinely corrupt file
    // fails again and the scan skips it, same as before.
    return parse_with_prefix(path, game_id, 0);
  }
}

}  // namespace engine
