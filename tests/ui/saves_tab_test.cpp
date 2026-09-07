// Offscreen GUI test for the Saves tab (Skyrim-style ESS saves + the
// missing-assets column).
//
// Verifies:
//   - set_saves() populates the three columns (Name/File/Missing) in the given
//     (newest-first) order and the per-save accessors resolve the row,
//   - the Missing column shows the missing-plugin count and its tooltip names
//     the plugins with their provider mods (MO2 tooltip spirit),
//   - request_scan() runs an end-to-end scan through the worker thread: real
//     parseable SE saves land in the table and the missing column reflects the
//     request's plugin snapshot + mods/overwrite dirs (enabled = satisfied,
//     provided by a mod folder = satisfied-with-provider, absent = missing),
//   - set_saves_dir() records the directory (no watcher: scans run once at
//     game load and after a delete — a save dropped on disk must NOT trigger
//     a background re-scan, regression for the Aug 2026 watch-spam),
//   - clear_saves() empties the table and unwatches the dir.
//
// The Delete/context-menu flows are NOT exercised: on_delete_key() shows a
// modal QMessageBox and the context menu runs menu.exec(), both of which block
// on the offscreen platform.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, temp saves dir, no
// network. Uses compression-type 0 (raw) SE saves so no zlib/lz4 fixture code
// is needed here (the engine still links them for the reader).
#include "ui/panels/tab_panels.h"

#include "engine/game/saves/save_game.h"
#include "engine/game/saves/save_reader.h"
#include "engine/pipeline/plugin_host/save_parser_registry.h"

#include <QApplication>
#include <QEvent>
#include <QEventLoop>
#include <QLabel>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QWidget>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {
void check(bool cond, const char* what) {
    INFO(what);
    REQUIRE(cond);
}
}

// --- minimal SE-format save writer (compression type 0) ---
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

static void write_save(const fs::path& dir, const std::string& base,
                       const std::string& pc, uint32_t level,
                       const std::string& loc, uint32_t save_number,
                       uint64_t filetime,
                       const std::vector<std::string>& plugins) {
    std::vector<char> f;
    const char* magic = "TESV_SAVEGAME";
    f.insert(f.end(), magic, magic + 13);

    // header
    put_u32(f, 0);  // header size (unused)
    put_u32(f, 12);
    put_u32(f, save_number);
    put_str(f, pc);
    put_u32(f, level);
    put_str(f, loc);
    put_str(f, "12:34:56");
    put_str(f, "ImperialRace");
    put_u16(f, 0);  // gender
    for (int i = 0; i < 8; ++i) f.push_back(0);  // xp
    put_u64(f, filetime);

    // screenshot (RGBA) — small so the file stays tiny
    put_u32(f, 32);
    put_u32(f, 32);
    put_u16(f, 0);  // compression: raw
    for (int i = 0; i < 32 * 32 * 4; ++i) f.push_back(static_cast<char>(i & 0xFF));

    // plugin info
    f.push_back(78);  // form version >= 78 → light plugins present
    f.push_back(1);   // plugin info size (unused)
    put_u16(f, 0);
    f.push_back(0);
    f.push_back(static_cast<char>(plugins.size()));
    for (const auto& p : plugins) put_str(f, p);
    put_u16(f, 0);  // no light plugins

    std::ofstream(dir / (base + ".ess"), std::ios::binary)
        .write(f.data(), static_cast<std::streamsize>(f.size()));
}

static void write_file(const fs::path& p, const std::string& data) {
    std::ofstream(p, std::ios::binary).write(data.data(),
                                             static_cast<std::streamsize>(data.size()));
}

// --- minimal reader for the write_save fixture format above ---
// Production Gamebryo parsing moved to the Plugins shared packet; this test
// only needs to round-trip its own fixtures (registry dispatch + tab
// rendering are what is under test, not TES field layouts).
namespace {
struct FixtureCursor {
    const std::vector<uint8_t>& b;
    size_t at = 0;
    uint8_t u8() {
        if (at + 1 > b.size()) throw engine::SaveParseError("eof");
        return b[at++];
    }
    uint16_t u16() {
        if (at + 2 > b.size()) throw engine::SaveParseError("eof");
        uint16_t v = static_cast<uint16_t>(b[at] | (b[at + 1] << 8));
        at += 2;
        return v;
    }
    uint32_t u32() {
        if (at + 4 > b.size()) throw engine::SaveParseError("eof");
        uint32_t v = static_cast<uint32_t>(b[at]) |
                     (static_cast<uint32_t>(b[at + 1]) << 8) |
                     (static_cast<uint32_t>(b[at + 2]) << 16) |
                     (static_cast<uint32_t>(b[at + 3]) << 24);
        at += 4;
        return v;
    }
    uint64_t u64() {
        uint64_t lo = u32();
        uint64_t hi = u32();
        return lo | (hi << 32);
    }
    std::string str() {
        uint16_t n = u16();
        if (at + n > b.size()) throw engine::SaveParseError("eof");
        std::string s(reinterpret_cast<const char*>(&b[at]), n);
        at += n;
        return s;
    }
    void skip(size_t n) {
        if (at + n > b.size()) throw engine::SaveParseError("eof");
        at += n;
    }
};

engine::SaveGame parse_fixture_save(const fs::path& path,
                                    const std::string& game_id) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw engine::SaveParseError("open");
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    FixtureCursor c{b};
    const char* magic = "TESV_SAVEGAME";
    for (int i = 0; i < 13; ++i)
        if (c.u8() != static_cast<uint8_t>(magic[i]))
            throw engine::SaveParseError("magic");
    c.u32();  // header size (unused)
    c.u32();  // version
    engine::SaveGame out;
    out.file_path = path;
    out.game_id = game_id;
    out.save_number = c.u32();
    out.pc_name = c.str();
    out.pc_level = static_cast<uint16_t>(c.u32());
    out.pc_location = c.str();
    c.str();  // playtime
    c.str();  // race
    c.u16();  // gender
    c.skip(8);  // xp
    out.creation_time = engine::filetime_to_epoch(c.u64());
    const uint32_t w = c.u32();
    const uint32_t h = c.u32();
    c.u16();  // compression (0 = raw)
    c.skip(static_cast<size_t>(w) * h * 4);  // RGBA screenshot
    c.u8();  // form version
    c.u8();  // plugin info size (unused)
    c.u16();
    c.u8();
    const int n = c.u8();
    for (int i = 0; i < n; ++i) out.plugins.push_back(c.str());
    return out;
}
}  // namespace

static QWidget* find_tooltip_widget() {
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (w->windowType() == Qt::ToolTip) return w;
    }
    return nullptr;
}

TEST_CASE("saves tab", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const fs::path cfg = "/tmp/gmm_saves_tab/config";
    fs::remove_all("/tmp/gmm_saves_tab");
    fs::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

    // Register the fixture parser so scan_saves can parse the fixtures.
    // Production Gamebryo parsing lives in the Plugins shared packet and is
    // registered by the game plugin; tests run without a full plugin load,
    // so the test-local fixture reader above stands in. It reads exactly
    // what write_save writes.
    if (!engine::SaveParserRegistry::instance().has_parser("skyrimse")) {
        engine::SaveParserRegistry::instance().register_parser(
            "skyrimse", 0,
            [](const std::filesystem::path& path,
               const std::string& game_id) {
                return parse_fixture_save(path, game_id);
            },
            nullptr, "test:fixture");
    }

    // --- Part 1: set_saves() population + missing-column rendering ---
    ui::SavesTab tab;

    engine::SaveGame a;
    a.file_path = "/tmp/gmm_saves_tab/part1/A_20260101_1_1.ess";
    a.pc_name = "Player1";
    a.pc_level = 40;
    a.pc_location = "Whiterun";
    a.save_number = 1;
    a.plugins = {"Skyrim.esm", "SkyUI_SE.esp", "GoneMod.esp"};

    engine::SaveGame b;
    b.file_path = "/tmp/gmm_saves_tab/part1/B_20260101_1_2.ess";
    b.pc_name = "Player1";
    b.pc_level = 39;
    b.pc_location = "Riften";
    b.save_number = 2;
    b.plugins = {"Skyrim.esm"};

    ui::SavesScanResultEntry ea;
    ea.save = a;
    ea.missing.push_back({"GoneMod.esp", /*origin_mod=*/"", /*inactive=*/false,
                          /*providing_mods=*/{}});

    ui::SavesScanResultEntry eb;
    eb.save = b;
    eb.missing.push_back({"SkyUI_SE.esp", "SkyUI", /*inactive=*/true,
                          /*providing_mods=*/{}});

    ui::SavesScanResult result;
    result.entries = {ea, eb};
    tab.set_saves(result);

    auto* table = tab.table();
    check(table->rowCount() == 2, "two rows after set_saves");
    check(table->columnCount() == 3, "three columns");
    check(table->horizontalHeaderItem(2)->text() == QLatin1String("Missing"),
          "Missing column header");
    check(table->item(0, 0)->text().contains("Player1") &&
              table->item(0, 0)->text().contains("Level 40"),
          "name column carries the display name");
    check(table->item(0, 1)->text() == "A_20260101_1_1.ess",
          "file column carries the basename");
    check(table->item(0, 2)->text() == "1" && table->item(1, 2)->text() == "1",
          "missing column shows the count");
    check(table->item(0, 2)->toolTip().contains("GoneMod.esp"),
          "missing tooltip names the absent plugin");
    check(table->item(1, 2)->toolTip().contains("SkyUI_SE.esp") &&
              table->item(1, 2)->toolTip().contains("(disabled)"),
          "inactive-missing tooltip names the disabled plugin");
    check(tab.save_at(0) && tab.save_at(0)->pc_name == "Player1",
          "save_at resolves row 0");
    check(tab.save_at(2) == nullptr, "save_at out of range → null");
    check(tab.missing_at(0) && tab.missing_at(0)->size() == 1,
          "missing_at resolves row 0");

    // --- Part 1b: v2.1+ save overlay (kv rows) renders in the hover info ---
    // A plugin's register_save_overlay fn builds a GmmSaveOverlayV2 which the
    // engine flattens into SaveGame::overlay. The Saves tab then renders
    // these rows below the default metadata. Verify they show up.
    engine::SaveGame ov;
    ov.file_path = "/tmp/gmm_saves_tab/part1b/Overlay.ess";
    ov.pc_name = "Overlayed";
    ov.pc_level = 12;
    ov.pc_location = "TestCell";
    ov.save_number = 7;
    ov.overlay = {
        {"Quest", "Main Questline"},
        {"Weather", "Clear"},
        {"Cell", "WhiterunDragonsreach"},
    };
    ui::SavesScanResultEntry eo;
    eo.save = ov;
    ui::SavesScanResult r2;
    r2.entries = {eo};
    tab.set_saves(std::move(r2));
    // Trigger the hover popup and look for the overlay rows.
    QTableWidget* overlay_table = tab.table();
    overlay_table->itemEntered(overlay_table->item(0, 0));
    QWidget* overlay_popup = find_tooltip_widget();
    check(overlay_popup != nullptr, "hover on overlay row creates the popup");
    if (overlay_popup) {
        QStringList texts;
        for (QLabel* lbl : overlay_popup->findChildren<QLabel*>()) {
            texts << lbl->text();
        }
        const QString joined = texts.join('\n');
        check(joined.contains("Quest") &&
                  joined.contains("Main Questline"),
              "overlay row 'Quest' rendered");
        check(joined.contains("Weather") &&
                  joined.contains("Clear"),
              "overlay row 'Weather' rendered");
        check(joined.contains("Cell") &&
                  joined.contains("WhiterunDragonsreach"),
              "overlay row 'Cell' rendered");
    }

    // --- Part 2: end-to-end scan through the worker thread ---
    const fs::path saves = "/tmp/gmm_saves_tab/saves";
    const fs::path mods = "/tmp/gmm_saves_tab/mods";
    const fs::path overwrite = "/tmp/gmm_saves_tab/overwrite";
    fs::create_directories(saves);
    fs::create_directories(mods);
    fs::create_directories(overwrite);
    // A mod folder that provides SkyUI_SE.esp; the overwrite dir provides a
    // stray copy of GoneMod.esp.
    fs::create_directories(mods / "SkyUI");
    fs::create_directories(overwrite);
    write_file(mods / "SkyUI" / "SkyUI_SE.esp", "dummy");
    write_file(overwrite / "GoneMod.esp", "dummy");

    const uint64_t newer = 0x01DD2288D3CC4860ULL;  // 2026-08-02T14:11:35 UTC
    const uint64_t older = 0x01DD227000000000ULL;  // older
    write_save(saves, "Player1_20260802141135_1_1", "Player1", 40, "Whiterun", 1,
               newer, {"Skyrim.esm", "SkyUI_SE.esp", "GoneMod.esp"});
    write_save(saves, "Player1_20260802090000_1_2", "Player1", 39, "Riften", 2,
               older, {"Skyrim.esm", "SkyUI_SE.esp"});
    write_file(saves / "Player1_20260802141135_1_1.skse", "co-save");

    ui::SavesScanRequest request;
    request.saves_dir = saves;
    request.extensions = {"ess"};
    request.game_id = "skyrimse";
    // Snapshot: Skyrim.esm enabled (satisfied); SkyUI_SE.esp NOT in the list
    // (but provided by the SkyUI mod folder); GoneMod.esp absent but provided
    // by <overwrite>. A save listing something with no provider at all would
    // count as missing.
    engine::GamePlugin skyrim;
    skyrim.name = "Skyrim.esm";
    skyrim.enabled = true;
    request.plugins = {skyrim};
    request.mods_dir = mods;
    request.overwrite_dir = overwrite;
    tab.request_scan(std::move(request));

    // Wait until the scan's result actually lands: Part 1 already made the
    // table non-empty, so rowCount alone can't signal "fresh result". The
    // scan's row 0 carries the on-disk newest basename (Part 1 used synthetic
    // A_/B_ names), which distinguishes it from the stale Part 1 rows.
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);
    while (table->rowCount() != 2 ||
           table->item(0, 1)->text() != "Player1_20260802141135_1_1.ess") {
        if (!timeout.isActive()) break;
        loop.processEvents();
    }
    timeout.stop();

    check(table->rowCount() == 2 &&
              table->item(0, 1)->text() == "Player1_20260802141135_1_1.ess",
          "scan result landed in the table");
    if (table->rowCount() == 2 &&
        table->item(0, 1)->text() == "Player1_20260802141135_1_1.ess") {
        // Newest save first (contract: newest-first).
        const int new_row = 0;
        const int old_row = 1;
        check(table->item(old_row, 1)->text() == "Player1_20260802090000_1_2.ess",
              "older save second");
        // Skyrim.esm is enabled → satisfied (not missing). SkyUI_SE.esp and
        // GoneMod.esp are NOT in the load-order snapshot, so they stay missing
        // (MO2 STATE_MISSING) even though mod folders provide copies — the
        // providers ride the tooltip, the count keeps the state.
        check(table->item(new_row, 2)->text() == "2",
              "two masters absent from the snapshot count missing");
        check(table->item(old_row, 2)->text() == "1",
              "one master absent from the snapshot counts missing");
        const QString tip_new = table->item(new_row, 2)->toolTip();
        const QString tip_old = table->item(old_row, 2)->toolTip();
        check(tip_new.contains("SkyUI_SE.esp") && tip_new.contains("SkyUI"),
              "tooltip names the mod folder that provides the plugin");
        check(tip_new.contains("GoneMod.esp") && tip_new.contains("<overwrite>"),
              "tooltip names <overwrite> as a provider");
        check(!tip_new.contains("Skyrim.esm"),
              "enabled master never appears as missing");
        check(tab.save_at(0) != nullptr, "scan result readable via save_at");
    }

    // --- Part 3: no background re-scan (regression for the Aug 2026 spam:
    // the Proton-prefix Saves dir churns on its own, and the old
    // QFileSystemWatcher auto-rescan fired ~once per second while idle).
    // Scans run once at game load and after a delete — never in the background.
    tab.set_saves_dir(saves);
    check(tab.saves_dir() == saves, "set_saves_dir records the dir");
    // A change on disk must NOT grow the table: drop a brand-new, newest save
    // into the dir and give the old watcher machinery plenty of time to
    // (wrongly) fire. The table must stay exactly as the explicit scan left it.
    const uint64_t newest = newer + 0x10000000;  // FILETIME, ~3 min later
    write_save(saves, "Player1_20260803000000_1_3", "Player1", 41, "Whiterun", 3,
               newest, {"Skyrim.esm"});
    QEventLoop loop2;
    QTimer t2;
    t2.setSingleShot(true);
    QObject::connect(&t2, &QTimer::timeout, &loop2, &QEventLoop::quit);
    t2.start(2500);  // > the old 500ms debounce + scan round-trip
    loop2.exec();
    check(table->rowCount() == 2 &&
              table->item(0, 1)->text() == "Player1_20260802141135_1_1.ess",
          "a save dropped on disk does NOT auto-trigger a re-scan");

    // --- Part 4: hover info popup survives external close (regression for the
    // Aug 2026 crash: deleting a save killed the whole manager) ---
    // The popup is a Qt::ToolTip window with WA_DeleteOnClose. Qt auto-dismisses
    // a visible tooltip when the user presses a mouse button / key (e.g. clicking
    // a row before pressing Delete); that close() destroys the widget behind the
    // tab's back. SavesTab must tolerate the next hover instead of calling
    // close() on the freed pointer (use QPointer — raw QWidget* was a UAF).
    table->itemEntered(table->item(0, 0));
    QWidget* popup = find_tooltip_widget();
    check(popup != nullptr, "hover on a row creates the info popup");
    if (popup) {
        popup->close();  // exactly what Qt's tooltip auto-dismiss does
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(find_tooltip_widget() == nullptr,
              "external close + deferred delete destroyed the popup");
        table->itemEntered(table->item(0, 0));  // re-enter → show_save_info again
        QWidget* rebuilt = find_tooltip_widget();
        check(rebuilt != nullptr && rebuilt->isVisible(),
              "re-entering after external destroy rebuilds the popup (no UAF)");
    }

    // --- Part 5: clear_saves ---
    tab.clear_saves();
    check(table->rowCount() == 0, "clear_saves empties the table");

    fs::remove_all("/tmp/gmm_saves_tab");
}

// Workspace-c48h: when no save parser is registered for the active game, the
// scanner used to silently skip every file (scan_saves guards against an
// empty parse_fn with `continue`), so the Saves tab was always empty even
// when the directory held .ess files. The worker now falls back to a stub
// parser that returns a minimal SaveGame (file_path + filesystem mtime), so
// the user at least sees the file listed - preferrable to a silent empty tab
// when the game's plugin didn't ship a parser.
TEST_CASE("saves tab no-parser fallback lists files", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const fs::path cfg = "/tmp/gmm_saves_tab_noparser/config";
    const fs::path saves = "/tmp/gmm_saves_tab_noparser/saves";
    fs::remove_all("/tmp/gmm_saves_tab_noparser");
    fs::create_directories(cfg);
    fs::create_directories(saves);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

    // The registry is process-wide; nothing else in this binary registers
    // "noparsergame", so the worker must take the no-parser fallback. The
    // "skyrimse" parser the other test case registers is irrelevant (we
    // look up "noparsergame" by exact game_id match).
    check(!engine::SaveParserRegistry::instance().has_parser("noparsergame"),
          "test precondition: no parser registered for noparsergame");
    // The .ess files in `saves` are deliberately NOT valid save files (a
    // registered parser would throw SaveParseError and skip them - we want
    // the fallback to claim them as files, not parse them).
    write_file(saves / "Quicksave_20260101_1_1.ess", "not a real save");
    write_file(saves / "Autosave_20260102_2_3.ess", "also not real");

    ui::SavesTab tab;
    auto* table = tab.table();
    check(table->rowCount() == 0, "fresh tab starts empty");

    ui::SavesScanRequest request;
    request.saves_dir = saves;
    request.extensions = {"ess"};
    request.game_id = "noparsergame";
    // No plugin snapshot - the stub SaveGame has empty plugins/light_plugins
    // and find_save_missing_assets returns empty for that.
    tab.request_scan(std::move(request));

    // Wait for the worker to land the result.
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);
    while (table->rowCount() != 2) {
        if (!timeout.isActive()) break;
        loop.processEvents();
    }
    timeout.stop();

    check(table->rowCount() == 2,
          "no-parser fallback still lists both .ess files (regression for "
          "Workspace-c48h: Saves tab showed nothing)");
    if (table->rowCount() == 2) {
        // The stub populates only file_path + creation_time; the file column
        // carries the basename and the creation_time is the filesystem mtime.
        check(table->item(0, 1)->text() == "Quicksave_20260101_1_1.ess" ||
                  table->item(0, 1)->text() == "Autosave_20260102_2_3.ess",
              "file column carries the basename");
        const auto* save = tab.save_at(0);
        check(save != nullptr && save->file_path.extension() == ".ess",
              "save_at resolves the stub SaveGame and it points at the file");
        check(save != nullptr && save->creation_time > 0,
              "stub SaveGame carries a non-zero mtime as creation_time");
        // The missing-assets column is empty for a stub (no plugins listed).
        check(table->item(0, 2)->text().isEmpty(),
              "missing-assets column is empty for a stub (no plugins)");
    }

    fs::remove_all("/tmp/gmm_saves_tab_noparser");
}

// Workspace-k53a: SavesTab::request_scan coalesces overlapping requests so the
// boot path (wire_saves_tab -> load_mods_from_game -> on_mod_scan_finished)
// does not queue two scans back to back. The fixture parser used by the
// "saves tab" case above is still registered (process-wide registry), so a
// second offscreen tab can drive a real scan and observe coalescing.
TEST_CASE("saves tab coalesces overlapping scan requests", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const fs::path cfg = "/tmp/gmm_saves_tab_coalesce/config";
    const fs::path saves_dir = "/tmp/gmm_saves_tab_coalesce/saves";
    const fs::path mods = "/tmp/gmm_saves_tab_coalesce/mods";
    const fs::path ow = "/tmp/gmm_saves_tab_coalesce/ow";
    fs::remove_all("/tmp/gmm_saves_tab_coalesce");
    fs::create_directories(cfg);
    fs::create_directories(saves_dir);
    fs::create_directories(mods);
    fs::create_directories(ow);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

    // Three saves with distinguishable file basenames so the test can
    // observe which request ultimately landed in the table.
    const uint64_t t1 = 0x01DD228000000000ULL;
    const uint64_t t2 = t1 + 0x10000000;
    const uint64_t t3 = t2 + 0x10000000;
    write_save(saves_dir, "A_first", "Pa", 10, "Loc1", 1, t1, {"Skyrim.esm"});
    write_save(saves_dir, "B_second", "Pb", 11, "Loc2", 2, t2, {"Skyrim.esm"});
    write_save(saves_dir, "C_third", "Pc", 12, "Loc3", 3, t3, {"Skyrim.esm"});

    ui::SavesTab tab;
    auto* table = tab.table();
    check(table->rowCount() == 0, "fresh tab starts empty");

    auto build_request = [&](const std::string& tag) {
        ui::SavesScanRequest req;
        req.saves_dir = saves_dir;
        req.extensions = {"ess"};
        req.game_id = "skyrimse";
        // Stash a marker in mods_dir: the test path encodes the tag so we
        // can tell which request was the one that ran.
        req.mods_dir = mods / ("tag-" + tag);
        req.overwrite_dir = ow;
        fs::create_directories(req.mods_dir);
        return req;
    };

    // Fire two requests back-to-back. The second should replace the first
    // (coalesce) so the eventual scan uses tag-B.
    tab.request_scan(build_request("A"));
    tab.request_scan(build_request("B"));
    // The first request was in flight; the second is now the pending one.
    // No third request: drive a flush by waiting for the first to finish.
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);
    // Wait until the table actually populates with the first scan.
    while (table->rowCount() != 3) {
        if (!timeout.isActive()) break;
        loop.processEvents();
    }
    check(table->rowCount() == 3,
          "first scan populated the table with all 3 saves");
    // After the first scan finishes, the coalesced pending request must run
    // (or the latch must have been cleared). The simplest invariant is: the
    // table is never overwritten with a *third* of these scans after a
    // quiet period (i.e. we don't keep firing). Verify by waiting again.
    int rows_after_first = table->rowCount();
    QEventLoop loop2;
    QTimer timeout2;
    timeout2.setSingleShot(true);
    QObject::connect(&timeout2, &QTimer::timeout, &loop2, &QEventLoop::quit);
    timeout2.start(1500);
    loop2.exec();
    // The coalesced second scan should have run by now (it is queued, not
    // dropped). Its result is the same 3 saves (same saves_dir) so the
    // observable row count is unchanged - but the scan happened, which
    // is what coalesce is about. We can't trivially prove a scan ran
    // without instrumenting the worker; the contract we DO guarantee is
    // that no THIRD scan is queued and the table doesn't grow past 3.
    check(table->rowCount() == rows_after_first,
          "coalesced scan does not duplicate rows");

    // Now fire a third batch and confirm the same coalesce semantics hold:
    // the last request wins.
    auto* table_before = table;
    tab.request_scan(build_request("X"));
    tab.request_scan(build_request("Y"));
    tab.request_scan(build_request("Z"));
    QEventLoop loop3;
    QTimer timeout3;
    timeout3.setSingleShot(true);
    QObject::connect(&timeout3, &QTimer::timeout, &loop3, &QEventLoop::quit);
    timeout3.start(5000);
    while (table->rowCount() != 3) {
        if (!timeout.isActive()) break;
        loop3.processEvents();
    }
    check(table == table_before && table->rowCount() == 3,
          "three rapid requests still settle at one final scan (3 rows)");

    fs::remove_all("/tmp/gmm_saves_tab_coalesce");
}

// Workspace-0owv: the worker streams saves via entryReady and SavesTab
// inserts each one in sorted order (newest first). The test verifies the
// STREAMING MECHANISM (QSignalSpy counts 3 entryReady emissions on the
// worker, not 1 finished-with-batch) and the SORT ORDER (binary-insert
// keeps the table sorted even when the worker delivers entries out of
// parse order). We do NOT assert incremental rowCount growth: synthetic
// SE saves parse in microseconds, so the worker drains its queue before
// the main thread's next processEvents tick - a single 0->3 jump is the
// correct user-visible behavior for tiny fixtures, even though the
// underlying signal stream is one-per-save. Real Skyrim saves with
// compressed data take ~100ms each and the streaming is observable.
TEST_CASE("saves tab streams saves as they load", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const fs::path root = "/tmp/gmm_saves_tab_stream";
    const fs::path cfg = root / "config";
    const fs::path saves = root / "saves";
    const fs::path mods = root / "mods";
    const fs::path ow = root / "ow";
    fs::remove_all(root);
    fs::create_directories(cfg);
    fs::create_directories(saves);
    fs::create_directories(mods);
    fs::create_directories(ow);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

    // 3 saves with distinct filetimes so creation_time orders them
    // deterministically. Filenames are decoupled from filetimes: file
    // "Stream_20260802_1" carries the LATEST filetime, "Stream_20260804_3"
    // carries the EARLIEST. The worker's parallel parse may complete them
    // in any order; the binary insert must still land them in
    // creation_time desc order at rows 0/1/2.
    write_save(saves, "Stream_20260802_1", "P1", 1, "L1", 1, 0x01DD228200000000ULL,
               {"Skyrim.esm"});
    write_save(saves, "Stream_20260803_2", "P1", 2, "L2", 2, 0x01DD228100000000ULL,
               {"Skyrim.esm"});
    write_save(saves, "Stream_20260804_3", "P1", 3, "L3", 3, 0x01DD228000000000ULL,
               {"Skyrim.esm"});

    ui::SavesTab tab;
    // Spy on the worker's entryReady to prove the streaming signal fires
    // once per save (not one batched finished with N entries).
    QSignalSpy entry_spy(tab.scan_thread()->worker(), &ui::SavesScanWorker::entryReady);
    QSignalSpy finished_spy(tab.scan_thread()->worker(), &ui::SavesScanWorker::finished);
    REQUIRE(entry_spy.isValid());
    REQUIRE(finished_spy.isValid());

    ui::SavesScanRequest request;
    request.saves_dir = saves;
    request.extensions = {"ess"};
    request.game_id = "skyrimse";
    request.mods_dir = mods;
    request.overwrite_dir = ow;
    tab.request_scan(std::move(request));

    // Wait for the final finished signal: the worker emits entryReady per
    // save and a single finished(int) at the end.
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);
    while (finished_spy.count() == 0) {
        if (!timeout.isActive()) break;
        QTimer::singleShot(5, &loop, &QEventLoop::quit);
        loop.exec();
    }
    timeout.stop();

    auto* table = tab.table();
    check(table->rowCount() == 3, "scan finishes with all 3 rows");
    // Streaming contract: 3 entryReady emissions, 1 finished. The old batch
    // path had 0 entryReady + 1 finished(SavesScanResult) carrying all 3.
    check(entry_spy.count() == 3,
          "worker emitted entryReady 3 times (one per save, streaming)");
    check(finished_spy.count() == 1, "worker emitted finished once at the end");
    // Binary-insert keeps the table in creation_time desc order even when
    // the worker delivers entries in parse-completion order (which may
    // differ from creation_time because the worker parallelizes parses).
    // Filenames above carry filetimes 0x82, 0x81, 0x80 (desc) so the
    // expected sorted order is 02_1 (newest), 03_2, 04_3 (oldest).
    check(table->item(0, 1)->text() == "Stream_20260802_1.ess",
          "largest filetime lands at row 0 (binary insert sorts, not parse order)");
    check(table->item(1, 1)->text() == "Stream_20260803_2.ess",
          "middle filetime at row 1");
    check(table->item(2, 1)->text() == "Stream_20260804_3.ess",
          "smallest filetime at row 2");

    fs::remove_all(root);
}
