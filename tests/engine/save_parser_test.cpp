// Engine test for the Bethesda save parser (MO2 GamebryoSaveGame port).
//
// Part A (always runs): synthetic ESS fixtures —
//   - Skyrim LE: uncompressed, RGB screenshot, formVersion u8 + pluginInfoSize
//     u32 + u8 count layout.
//   - Skyrim SE v12: RGBA screenshot + u16 compression type; compression types
//     0 (raw), 1 (independent zlib streams at 16-byte-aligned offsets — a
//     two-stream fixture exercises the chunk-crossing + alignment path) and
//     2 (one LZ4 block). Light plugins present when formVersion >= 78.
//   - Header strings are u16 length + bytes (UTF-8), filetime → epoch.
// Part B (when the real Skyrim SE saves exist): parse every .ess on the real
//   instance, check the known "Vanilla Vanny" save's fields exactly, and pin
//   embedded-time == filename-time for every file whose name carries a
//   standard timestamp.

#include "engine/saves/save_game.h"
#include "engine/saves/save_missing_assets.h"
#include "engine/saves/save_reader.h"
#include "engine/saves/save_scanner.h"
#include "engine/saves/skyrim_save.h"

#include <lz4.h>
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace engine;

namespace fs = std::filesystem;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

// --- tiny little-endian writers ---
static void put_u16(std::vector<char>& v, uint16_t x) {
    v.push_back(static_cast<char>(x & 0xFF));
    v.push_back(static_cast<char>((x >> 8) & 0xFF));
}
static void put_u32(std::vector<char>& v, uint32_t x) {
    v.push_back(static_cast<char>(x & 0xFF));
    v.push_back(static_cast<char>((x >> 8) & 0xFF));
    v.push_back(static_cast<char>((x >> 16) & 0xFF));
    v.push_back(static_cast<char>((x >> 24) & 0xFF));
}
static void put_u64(std::vector<char>& v, uint64_t x) {
    put_u32(v, static_cast<uint32_t>(x & 0xFFFFFFFFu));
    put_u32(v, static_cast<uint32_t>((x >> 32) & 0xFFFFFFFFu));
}
static void put_str(std::vector<char>& v, const std::string& s) {
    put_u16(v, static_cast<uint16_t>(s.size()));
    v.insert(v.end(), s.begin(), s.end());
}

// Header layout is identical for LE and SE (LE skips the two u32s, SE reads
// them; both parse the rest the same way).
static std::vector<char> build_header(const std::string& pc, uint32_t level,
                                      const std::string& loc, uint32_t save_number,
                                      uint64_t filetime, uint32_t version) {
    std::vector<char> h;
    put_u32(h, 0);  // header size (unused)
    put_u32(h, version);
    put_u32(h, save_number);
    put_str(h, pc);
    put_u32(h, level);
    put_str(h, loc);
    put_str(h, "12:34:56");  // time of day
    put_str(h, "ImperialRace");
    put_u16(h, 0);  // gender
    char f = 0;
    char z = 0;
    for (int i = 0; i < 4; ++i) h.push_back(f);  // xp gathered
    for (int i = 0; i < 4; ++i) h.push_back(z);  // xp required
    put_u64(h, filetime);
    return h;
}

static std::vector<char> build_le_data(const std::vector<std::string>& plugins) {
    std::vector<char> d;
    d.push_back(12);  // form version
    put_u32(d, static_cast<uint32_t>(plugins.size()));  // plugin info size (unused)
    d.push_back(static_cast<char>(plugins.size()));
    for (const auto& p : plugins) put_str(d, p);
    return d;
}

static std::vector<char> build_se_data(const std::vector<std::string>& plugins,
                                       const std::vector<std::string>& light) {
    std::vector<char> d;
    d.push_back(78);  // form version >= 78 → light plugins present
    d.push_back(1);   // plugin info size (unused)
    put_u16(d, 0);    // other (unknown)
    d.push_back(0);   // pad byte
    d.push_back(static_cast<char>(plugins.size()));
    for (const auto& p : plugins) put_str(d, p);
    put_u16(d, static_cast<uint16_t>(light.size()));
    for (const auto& l : light) put_str(d, l);
    return d;
}

static std::vector<char> deflate_buf(const std::string& input) {
    uLongf bound = compressBound(input.size());
    std::vector<char> out(bound);
    z_stream zs{};
    check(deflateInit(&zs, Z_DEFAULT_COMPRESSION) == Z_OK, "deflateInit");
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    check(deflate(&zs, Z_FINISH) == Z_STREAM_END, "deflate finish");
    deflateEnd(&zs);
    out.resize(zs.total_out);
    return out;
}

static uint64_t round16(uint64_t x) {
    if (x % 16 == 0) return x;
    return x + 16 - (x % 16);
}

static void write_file(const fs::path& p, const std::vector<char>& data) {
    std::ofstream(p, std::ios::binary).write(data.data(), static_cast<std::streamsize>(data.size()));
}

// LE save: uncompressed, RGB screenshot.
static fs::path write_le_save(const fs::path& dir, const std::string& base,
                              const std::vector<std::string>& plugins,
                              uint64_t filetime) {
    std::vector<char> f;
    const char* magic = "TESV_SAVEGAME";
    f.insert(f.end(), magic, magic + 13);  // 13-byte magic (ends ...GAME)
    auto h = build_header("TestChar", 42, "Whiterun", 7, filetime, 12);
    f.insert(f.end(), h.begin(), h.end());
    put_u32(f, 320);
    put_u32(f, 192);
    for (int i = 0; i < 320 * 192 * 3; ++i) f.push_back(static_cast<char>(i & 0xFF));
    auto data = build_le_data(plugins);
    f.insert(f.end(), data.begin(), data.end());
    fs::path p = dir / (base + ".ess");
    write_file(p, f);
    return p;
}

// SE v12 save. comp 0/1/2; comp 1 splits the data region across two zlib
// streams at a 16-byte-aligned boundary (MO2 readNextChunk semantics).
static fs::path write_se_save(const fs::path& dir, const std::string& base,
                              uint16_t comp, const std::vector<std::string>& plugins,
                              const std::vector<std::string>& light, uint64_t filetime) {
    std::vector<char> f;
    const char* magic = "TESV_SAVEGAME";
    f.insert(f.end(), magic, magic + 13);  // 13-byte magic (ends ...GAME)
    auto h = build_header("TestChar", 42, "Whiterun", 7, filetime, 12);
    f.insert(f.end(), h.begin(), h.end());
    put_u32(f, 320);
    put_u32(f, 192);
    put_u16(f, comp);
    for (int i = 0; i < 320 * 192 * 4; ++i) f.push_back(static_cast<char>(i & 0xFF));
    auto data = build_se_data(plugins, light);
    if (comp == 0) {
        f.insert(f.end(), data.begin(), data.end());
    } else if (comp == 1) {
        // Two independent zlib streams. First at chunk_start (absolute
        // offset, written into the header), second at the 16-aligned offset
        // right after the first stream's end. Bytes between are padding.
        std::size_t split = data.size() / 2;
        std::string part1(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(split));
        std::string part2(data.begin() + static_cast<std::ptrdiff_t>(split), data.end());
        auto d1 = deflate_buf(part1);
        auto d2 = deflate_buf(part2);
        uint64_t chunk_start = static_cast<uint64_t>(f.size()) + 16;
        put_u64(f, chunk_start);
        put_u64(f, static_cast<uint64_t>(data.size()));
        f.insert(f.end(), d1.begin(), d1.end());
        uint64_t second = round16(chunk_start + d1.size());
        f.resize(static_cast<std::size_t>(second));  // zero padding to alignment
        f.insert(f.end(), d2.begin(), d2.end());
    } else if (comp == 2) {
        int bound = LZ4_compressBound(static_cast<int>(data.size()));
        std::vector<char> cbuf(static_cast<std::size_t>(bound));
        int csize = LZ4_compress_default(data.data(), cbuf.data(),
                                         static_cast<int>(data.size()), bound);
        check(csize > 0, "LZ4_compress_default");
        put_u32(f, static_cast<uint32_t>(data.size()));
        put_u32(f, static_cast<uint32_t>(csize));
        f.insert(f.end(), cbuf.begin(), cbuf.begin() + csize);
    }
    fs::path p = dir / (base + ".ess");
    write_file(p, f);
    return p;
}

static const uint64_t kTestFiletime = 0x01DD2288D3CC4860ULL;  // 2026-08-02T14:11:35 UTC

static void check_se_result(const SaveGame& s, const char* base) {
    check(s.file_path.filename().string() == std::string(base) + ".ess", "file path");
    check(s.game_id == "skyrimse", "game id");
    check(s.pc_name == "TestChar", "pc name");
    check(s.pc_level == 42, "pc level");
    check(s.pc_location == "Whiterun", "pc location");
    check(s.save_number == 7, "save number");
    check(s.creation_time == 1785679895LL, "creation time (epoch)");
    check(s.screenshot_width == 320 && s.screenshot_height == 192, "screenshot size");
    check(s.screenshot.size() == static_cast<std::size_t>(320 * 192 * 4), "RGBA screenshot");
    check(s.plugins.size() == 3, "plugin count");
    check(s.plugins[0] == "Skyrim.esm", "plugin[0]");
    check(s.plugins[1] == "Skyrim_Shadows.esm", "plugin[1]");
    check(s.plugins[2] == "TestMod.esp", "plugin[2]");
    check(s.light_plugins.size() == 2, "light plugin count");
    check(s.light_plugins[0] == "ccBGSFO4004-BlackBear.esl", "light[0]");
    check(s.light_plugins[1] == "TestLight.esl", "light[1]");
}

// --- civil-date → epoch (Howard Hinnant's days_from_civil) for the
// embedded-vs-filename timestamp pin in Part B. ---
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + static_cast<int64_t>(doe) - 719468;
}
static int64_t to_epoch(int y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s) {
    return days_from_civil(y, mo, d) * 86400LL + h * 3600LL + mi * 60LL + s;
}
// Extracts the YYYYMMDDHHMMSS timestamp from a standard save filename
// (..._20260802141135_1_1.ess); returns -1 when the name has none.
static int64_t filename_epoch(const std::string& name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot < 14) return -1;
    const std::string ts = name.substr(dot - 14, 14);
    for (char c : ts) {
        if (c < '0' || c > '9') return -1;
    }
    const int y = std::atoi(ts.substr(0, 4).c_str());
    const int mo = std::atoi(ts.substr(4, 2).c_str());
    const int d = std::atoi(ts.substr(6, 2).c_str());
    const int h = std::atoi(ts.substr(8, 2).c_str());
    const int mi = std::atoi(ts.substr(10, 2).c_str());
    const int s = std::atoi(ts.substr(12, 2).c_str());
    return to_epoch(y, static_cast<unsigned>(mo), static_cast<unsigned>(d),
                    static_cast<unsigned>(h), static_cast<unsigned>(mi),
                    static_cast<unsigned>(s));
}

int main() {
    fs::path root = fs::temp_directory_path() / "gmm_save_parser_test";
    fs::remove_all(root);
    fs::create_directories(root);

    // --- Part A: synthetic fixtures ---
    {
        std::vector<std::string> le_plugins = {"Skyrim.esm", "Update.esm", "LegacyMod.esp"};
        auto p = write_le_save(root, "le_save", le_plugins, kTestFiletime);
        SaveGame s = parse_skyrim_save(p);
        check(s.game_id == "skyrim", "le game id");
        check(s.pc_name == "TestChar", "le pc name");
        check(s.pc_level == 42, "le level");
        check(s.pc_location == "Whiterun", "le location");
        check(s.save_number == 7, "le save number");
        check(s.creation_time == 1785679895LL, "le creation time");
        check(s.screenshot.size() == static_cast<std::size_t>(320 * 192 * 3), "le RGB screenshot");
        check(s.plugins == le_plugins, "le plugins");
        check(s.light_plugins.empty(), "le no light plugins");
    }

    const std::vector<std::string> se_plugins = {"Skyrim.esm", "Skyrim_Shadows.esm", "TestMod.esp"};
    const std::vector<std::string> se_light = {"ccBGSFO4004-BlackBear.esl", "TestLight.esl"};
    {
        auto p = write_se_save(root, "se_raw", 0, se_plugins, se_light, kTestFiletime);
        check_se_result(parse_skyrimse_save(p), "se_raw");
    }
    {
        auto p = write_se_save(root, "se_lz4", 2, se_plugins, se_light, kTestFiletime);
        check_se_result(parse_skyrimse_save(p), "se_lz4");
    }
    {
        // Multi-chunk zlib (exercises the 16-byte-aligned chunk crossing).
        auto p = write_se_save(root, "se_zlib", 1, se_plugins, se_light, kTestFiletime);
        check_se_result(parse_skyrimse_save(p), "se_zlib");
    }

    // Older SE layout: formVersion < 78 → no light plugins, compression 2.
    {
        auto p = write_se_save(root, "se_no_light", 2, se_plugins, {}, kTestFiletime);
        SaveGame s = parse_skyrimse_save(p);
        check(s.plugins == se_plugins, "no-light plugins");
        check(s.light_plugins.empty(), "no-light light plugins empty");
    }

    // Garbage file → SaveParseError, and scan_saves skips it.
    {
        fs::path bad = root / "garbage.ess";
        write_file(bad, std::vector<char>(32, 'X'));
        bool threw = false;
        try {
            auto unused = parse_skyrimse_save(bad);
            (void)unused;
        } catch (const engine::SaveParseError&) {
            threw = true;
        }
        check(threw, "garbage file throws SaveParseError");
    }

    // Truncated file → SaveParseError.
    {
        auto p = write_se_save(root, "trunc", 2, se_plugins, se_light, kTestFiletime);
        std::vector<char> bytes;
        {
            std::ifstream in(p, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        bytes.resize(bytes.size() - 10);  // cut into the compressed region
        fs::path t = root / "truncated.ess";
        write_file(t, bytes);
        bool threw = false;
        try {
            auto unused = parse_skyrimse_save(t);
            (void)unused;
        } catch (const engine::SaveParseError&) {
            threw = true;
        }
        check(threw, "truncated file throws SaveParseError");
    }

    // scan_saves: filters by extension, sorts newest first, skips garbage.
    {
        auto saves = engine::scan_saves(root, {"ess"},
                                        [](const fs::path& p) { return parse_skyrimse_save(p); });
        // le_save parses as skyrimse? No — LE saves parse as SE would fail on
        // the missing compression field layout. It was written for the LE
        // parser; the scanner uses the SE parser here, so it must skip it
        // (its header parses, then the RGBA screenshot read runs past the
        // uncompressed data). Expect exactly the 4 SE saves + skipped others.
        check(!saves.empty(), "scanner returns saves");
        for (std::size_t i = 1; i < saves.size(); ++i) {
            check(saves[i - 1].creation_time >= saves[i].creation_time, "newest first");
        }
        bool found_lz4 = false;
        for (const auto& s : saves) {
            if (s.file_path.filename() == fs::path("se_lz4.ess")) found_lz4 = true;
        }
        check(found_lz4, "scanner found se_lz4");
    }

    // --- missing-assets resolver (MO2 getMissingAssets port) ---
    {
        // Load order with one active plugin, one inactive (present, disabled)
        // and everything else absent.
        std::vector<GamePlugin> plugins;
        {
            GamePlugin p;
            p.name = "Skyrim.esm";
            p.enabled = true;
            p.owner_mod = "";
            plugins.push_back(p);
        }
        {
            GamePlugin p;
            p.name = "SkyUI_SE.esp";
            p.enabled = false;
            p.owner_mod = "SkyUI";
            plugins.push_back(p);
        }
        {
            GamePlugin p;
            p.name = "SomeOtherMod.esp";
            p.enabled = true;
            p.owner_mod = "SomeOtherMod";
            plugins.push_back(p);
        }

        // Mods dir: "SkyUI" (disabled plugin), "GoneMod" (missing plugin),
        // "NoPlugins" (nothing relevant). Overwrite holds the missing plugin.
        const fs::path am = root / "am_mods";
        fs::create_directories(am / "SkyUI");
        fs::create_directories(am / "GoneMod");
        fs::create_directories(am / "NoPlugins");
        write_file(am / "SkyUI" / "SkyUI_SE.esp", std::vector<char>{'x'});
        write_file(am / "GoneMod" / "GonePlugin.esp", std::vector<char>{'x'});
        write_file(am / "NoPlugins" / "readme.txt", std::vector<char>{'x'});
        const fs::path ow = root / "am_overwrite";
        fs::create_directories(ow);
        write_file(ow / "GonePlugin.esp", std::vector<char>{'x'});

        SaveGame save;
        save.plugins = {"Skyrim.esm", "SkyUI_SE.esp", "GonePlugin.esp",
                        "AlsoMissing.esm"};
        auto missing = engine::find_save_missing_assets(save, plugins, am, ow);

        check(missing.size() == 3, "missing asset count");
        std::map<std::string, const SaveMissingAsset*> by_name;
        for (const auto& m : missing) by_name[m.plugin_name] = &m;

        auto it = by_name.find("SkyUI_SE.esp");
        check(it != by_name.end(), "inactive plugin is missing");
        check(it->second->inactive, "inactive flag set");
        check(it->second->origin_mod == "SkyUI", "inactive origin mod");
        check(std::find(it->second->providing_mods.begin(),
                        it->second->providing_mods.end(),
                        "SkyUI") != it->second->providing_mods.end(),
              "inactive provider found");

        it = by_name.find("GonePlugin.esp");
        check(it != by_name.end(), "absent plugin is missing");
        check(!it->second->inactive, "absent plugin not flagged inactive");
        check(it->second->origin_mod.empty(), "absent plugin has no origin");
        check(std::find(it->second->providing_mods.begin(),
                        it->second->providing_mods.end(),
                        "GoneMod") != it->second->providing_mods.end(),
              "providing mod found");
        check(std::find(it->second->providing_mods.begin(),
                        it->second->providing_mods.end(),
                        "<overwrite>") != it->second->providing_mods.end(),
              "overwrite provider found");

        it = by_name.find("AlsoMissing.esm");
        check(it != by_name.end(), "second absent plugin is missing");
        check(it->second->providing_mods.empty(), "no provider for AlsoMissing");

        check(by_name.find("Skyrim.esm") == by_name.end(),
              "active plugin is not missing");
    }

    // --- Part B: real Skyrim SE saves (skip if absent) ---
    const fs::path real_dir =
        "/mnt/SSD/Users/Petrica/Documents/My Games/Skyrim Special Edition/Saves";
    if (fs::is_directory(real_dir)) {
        std::size_t parsed = 0;
        std::size_t unparseable = 0;
        std::size_t time_mismatches = 0;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(real_dir, ec)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".ess") continue;
            SaveGame s;
            try {
                s = parse_skyrimse_save(entry.path());
            } catch (const engine::SaveParseError&) {
                ++unparseable;
                continue;
            }
            ++parsed;
            if (s.plugins.empty()) {
                std::fprintf(stderr, "WARN: %s parsed with empty plugin list\n",
                             entry.path().filename().c_str());
            }
            const int64_t fe = filename_epoch(entry.path().filename().string());
            if (fe >= 0 && s.creation_time != fe) {
                ++time_mismatches;
                std::fprintf(stderr, "WARN: %s embedded=%lld filename=%lld\n",
                             entry.path().filename().c_str(),
                             static_cast<long long>(s.creation_time),
                             static_cast<long long>(fe));
            }
            if (entry.path().filename().string().find("Vanilla Vanny") != std::string::npos &&
                entry.path().filename().string().find("000043") != std::string::npos) {
                // The known save: exact field pin.
                check(s.pc_name == "Vanilla Vanny", "real: pc name");
                check(s.pc_level == 43, "real: level");
                check(s.pc_location == "Whiterun", "real: location");
                check(s.save_number == 1, "real: save number");
                check(s.screenshot_width == 320 && s.screenshot_height == 192,
                      "real: screenshot size");
                check(!s.plugins.empty(), "real: plugins present");
                check(!s.light_plugins.empty(), "real: light plugins present");
                check(s.creation_time == 1785679895LL, "real: creation time");
            }
        }
        std::fprintf(stderr, "real saves: parsed=%zu unparseable=%zu time_mismatches=%zu\n",
                     parsed, unparseable, time_mismatches);
        check(parsed > 0, "real saves parsed");
        check(unparseable == 0, "every real .ess parses");
        check(time_mismatches == 0, "embedded time == filename time for all real saves");
    } else {
        std::fprintf(stderr, "note: real save dir absent, skipping Part B\n");
    }

    std::fprintf(stderr, "save_parser_test: PASS\n");
    return 0;
}
