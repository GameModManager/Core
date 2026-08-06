#include "engine/saves/skyrim_save.h"

#include "engine/saves/save_reader.h"

namespace engine {

namespace {

// Reads the shared LE/SE header block (everything up to and including the
// FILETIME), leaving the cursor just past it. Returns the header version
// (MO2 SkyrimSESaveGame reads it and later tests `version == 12` for the
// SE-specific alpha/compression fields).
std::uint32_t read_header(SaveReader& r, SaveGame& out) {
    r.skip(4);  // header size (does NOT bound the strings; MO2 ignores it)
    std::uint32_t version = r.u32();
    out.save_number = r.u32();
    out.pc_name = r.wstring();
    std::uint32_t level = r.u32();
    out.pc_level = static_cast<std::uint16_t>(level);
    out.pc_location = r.wstring();
    r.wstring();  // time of day
    r.wstring();  // race
    r.skip(2);    // player gender (0 = male)
    r.skip(8);    // experience gathered, experience required (2 floats)
    out.creation_time = filetime_to_epoch(r.u64());
    return version;
}

// Reads `count` u16-length strings (MO2 readPluginData).
std::vector<std::string> read_plugin_list(SaveReader& r, std::size_t count) {
    std::vector<std::string> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(r.wstring());
    }
    return out;
}

}  // namespace

SaveGame parse_skyrim_save(const std::filesystem::path& path) {
    SaveReader r(path, "TESV_SAVEGAME");
    SaveGame out;
    out.game_id = "skyrim";
    out.file_path = path;
    read_header(r, out);

    std::uint32_t w = r.u32();
    std::uint32_t h = r.u32();
    // LE: RGB, no alpha, no compression-type field
    // (MO2 SkyrimSaveGame::fetchDataFields).
    std::string pixels = r.read_bytes(static_cast<std::size_t>(w) * h * 3);
    out.screenshot.assign(pixels.begin(), pixels.end());
    out.screenshot_width = static_cast<int>(w);
    out.screenshot_height = static_cast<int>(h);

    r.begin_compressed(0);
    r.skip(1);  // form version
    r.skip(4);  // plugin info size (u32 in LE)
    out.plugins = read_plugin_list(r, r.u8());
    // LE has no light plugins (MO2 never reads them for LE).
    return out;
}

SaveGame parse_skyrimse_save(const std::filesystem::path& path, std::string game_id) {
    SaveReader r(path, "TESV_SAVEGAME");
    SaveGame out;
    out.game_id = std::move(game_id);
    out.file_path = path;
    std::uint32_t version = read_header(r, out);

    std::uint32_t w = r.u32();
    std::uint32_t h = r.u32();
    // SE version 12 adds a u16 compression type and an alpha channel
    // (MO2 SkyrimSESaveGame::fetchDataFields).
    bool alpha = false;
    std::uint16_t compression = 0;
    if (version == 12) {
        compression = r.u16();
        alpha = true;
    }
    std::string screenshot =
        r.read_bytes(static_cast<std::size_t>(w) * h * (alpha ? 4u : 3u));
    out.screenshot.assign(screenshot.begin(), screenshot.end());
    out.screenshot_width = static_cast<int>(w);
    out.screenshot_height = static_cast<int>(h);

    r.begin_compressed(compression);
    std::uint8_t save_game_version = r.u8();
    r.u8();  // plugin info size (u8 in SE)
    r.u16();  // "other" (unknown)
    r.skip(1);  // pad byte before the plugin count
    out.plugins = read_plugin_list(r, r.u8());
    if (save_game_version >= 78) {
        out.light_plugins = read_plugin_list(r, r.u16());
    }
    return out;
}

}  // namespace engine
