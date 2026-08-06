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
//   - set_saves_dir() arms the directory watcher and a change to the watched
//     dir triggers the 500ms-debounced refresh_requested() (MO2 savestab
//     behavior),
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

#include <QApplication>
#include <QEvent>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QWidget>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int failures = 0;
static int passes = 0;
static void check(bool cond, const char* what) {
    std::printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    if (cond)
        ++passes;
    else
        ++failures;
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

static QWidget* find_tooltip_widget() {
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (w->windowType() == Qt::ToolTip) return w;
    }
    return nullptr;
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const fs::path cfg = "/tmp/gmm_saves_tab/config";
    fs::remove_all("/tmp/gmm_saves_tab");
    fs::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

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

    // --- Part 3: directory watcher → debounced refresh_requested ---
    tab.set_saves_dir(saves);
    check(tab.watching(), "watcher armed on set_saves_dir");
    QSignalSpy spy(&tab, &ui::SavesTab::refresh_requested);
    write_file(saves / "new_save.ess", "x");  // a change in the watched dir
    QEventLoop loop2;
    QTimer t2;
    t2.setSingleShot(true);
    QObject::connect(&t2, &QTimer::timeout, &loop2, &QEventLoop::quit);
    t2.start(3000);
    while (spy.count() == 0 && t2.isActive()) loop2.processEvents();
    t2.stop();
    check(spy.count() >= 1, "dir change → (debounced) refresh_requested");

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
    check(!tab.watching(), "clear_saves unwatches the dir");

    std::printf("%d passed, %d failed\n", passes, failures);
    fs::remove_all("/tmp/gmm_saves_tab");
    return failures == 0 ? 0 : 1;
}
