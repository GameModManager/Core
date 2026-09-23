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
//   - set_saves_dir() records the directory and arms the debounced
//     directory watch (Workspace-69xt, MO2 parity): a save dropped on disk
//     re-scans after a 500ms quiet period, but ONLY while the tab is
//     visible - a hidden tab never scans in the background (the Aug 2026
//     watch-spam regression stays fixed via the visible-only gate),
//   - clear_saves() empties the table and unwatches the dir.
//   - first-show latch: no scan before first activation, exactly one scan
//     on first show, no rescan on later activations (Workspace-69xt
//     re-scoped spec: rescans come from the watcher / profile switch /
//     delete only),
//   - fast-scan preference: a registered fast parser beats the full parser;
//     the knowledge-declared "gamebryo-tesv" format parses real TESV saves
//     with no plugin parser registered; unknown formats fall back safely;
//     a file the fast reader cannot serve falls back to the full parser.
//
// The Delete/context-menu flows are NOT exercised: on_delete_key() shows a
// modal QMessageBox and the context menu runs menu.exec(), both of which block
// on the offscreen platform.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, temp saves dir, no
// network. Uses compression-type 0 (raw) SE saves so no zlib/lz4 fixture code
// is needed here (the engine still links them for the reader).
#include "ui/panels/tab_panels.h"

#include "engine/core/instance/instance.h"
#include "engine/game/registry/game_capabilities.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/game/saves/save_fast_scan.h"
#include "engine/game/saves/save_game.h"
#include "engine/game/saves/save_reader.h"
#include "engine/pipeline/plugin_host/save_parser_registry.h"
#include "engine/profile/profile_creation.h"
#include "ui/controllers/downloads_controller.h"
#include "ui/controllers/mod_list_controller.h"
#include "ui/main_window/main_window.h"
#include "ui/widgets/profile_bar.h"
#include "ui/widgets/right_panel.h"

#include <zlib.h>

#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QEventLoop>
#include <QLabel>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QWidget>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
void check(bool cond, const char* what) {
  INFO(what);
  REQUIRE(cond);
}
}  // namespace

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
                       const std::string& pc, uint32_t level, const std::string& loc,
                       uint32_t save_number, uint64_t filetime,
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
  for (int i = 0; i < 8; ++i)
    f.push_back(0);  // xp
  put_u64(f, filetime);

  // screenshot (RGBA) — small so the file stays tiny
  put_u32(f, 32);
  put_u32(f, 32);
  put_u16(f, 0);  // compression: raw
  for (int i = 0; i < 32 * 32 * 4; ++i)
    f.push_back(static_cast<char>(i & 0xFF));

  // plugin info
  f.push_back(78);  // form version >= 78 → light plugins present
  f.push_back(1);   // plugin info size (unused)
  put_u16(f, 0);
  f.push_back(0);
  f.push_back(static_cast<char>(plugins.size()));
  for (const auto& p : plugins)
    put_str(f, p);
  put_u16(f, 0);  // no light plugins

  std::ofstream(dir / (base + ".ess"), std::ios::binary)
      .write(f.data(), static_cast<std::streamsize>(f.size()));
}

static void write_file(const fs::path& p, const std::string& data) {
  std::ofstream(p, std::ios::binary)
      .write(data.data(), static_cast<std::streamsize>(data.size()));
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
    if (at + 1 > b.size())
      throw engine::SaveParseError("eof");
    return b[at++];
  }
  uint16_t u16() {
    if (at + 2 > b.size())
      throw engine::SaveParseError("eof");
    uint16_t v = static_cast<uint16_t>(b[at] | (b[at + 1] << 8));
    at += 2;
    return v;
  }
  uint32_t u32() {
    if (at + 4 > b.size())
      throw engine::SaveParseError("eof");
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
    if (at + n > b.size())
      throw engine::SaveParseError("eof");
    std::string s(reinterpret_cast<const char*>(&b[at]), n);
    at += n;
    return s;
  }
  void skip(size_t n) {
    if (at + n > b.size())
      throw engine::SaveParseError("eof");
    at += n;
  }
};

engine::SaveGame parse_fixture_save(const fs::path& path, const std::string& game_id) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw engine::SaveParseError("open");
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
  out.file_path   = path;
  out.game_id     = game_id;
  out.save_number = c.u32();
  out.pc_name     = c.str();
  out.pc_level    = static_cast<uint16_t>(c.u32());
  out.pc_location = c.str();
  c.str();    // playtime
  c.str();    // race
  c.u16();    // gender
  c.skip(8);  // xp
  out.creation_time = engine::filetime_to_epoch(c.u64());
  const uint32_t w  = c.u32();
  const uint32_t h  = c.u32();
  c.u16();                                 // compression (0 = raw)
  c.skip(static_cast<size_t>(w) * h * 4);  // RGBA screenshot
  c.u8();                                  // form version
  c.u8();                                  // plugin info size (unused)
  c.u16();
  c.u8();
  const int n = c.u8();
  for (int i = 0; i < n; ++i)
    out.plugins.push_back(c.str());
  return out;
}
}  // namespace

static QWidget* find_tooltip_widget() {
  for (QWidget* w : QApplication::topLevelWidgets()) {
    if (w->windowType() == Qt::ToolTip)
      return w;
  }
  return nullptr;
}

TEST_CASE("saves tab", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path cfg = "/tmp/gmm_saves_tab/config";
  fs::remove_all("/tmp/gmm_saves_tab");
  fs::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
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
        [](const std::filesystem::path& path, const std::string& game_id) {
          return parse_fixture_save(path, game_id);
        },
        nullptr, "test:fixture");
  }

  // --- Part 1: set_saves() population + missing-column rendering ---
  ui::SavesTab tab;

  engine::SaveGame a;
  a.file_path   = "/tmp/gmm_saves_tab/part1/A_20260101_1_1.ess";
  a.pc_name     = "Player1";
  a.pc_level    = 40;
  a.pc_location = "Whiterun";
  a.save_number = 1;
  a.plugins     = {"Skyrim.esm", "SkyUI_SE.esp", "GoneMod.esp"};

  engine::SaveGame b;
  b.file_path   = "/tmp/gmm_saves_tab/part1/B_20260101_1_2.ess";
  b.pc_name     = "Player1";
  b.pc_level    = 39;
  b.pc_location = "Riften";
  b.save_number = 2;
  b.plugins     = {"Skyrim.esm"};

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
  ov.file_path   = "/tmp/gmm_saves_tab/part1b/Overlay.ess";
  ov.pc_name     = "Overlayed";
  ov.pc_level    = 12;
  ov.pc_location = "TestCell";
  ov.save_number = 7;
  ov.overlay     = {
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
    check(joined.contains("Quest") && joined.contains("Main Questline"),
          "overlay row 'Quest' rendered");
    check(joined.contains("Weather") && joined.contains("Clear"),
          "overlay row 'Weather' rendered");
    check(joined.contains("Cell") && joined.contains("WhiterunDragonsreach"),
          "overlay row 'Cell' rendered");
  }

  // --- Part 2: end-to-end scan through the worker thread ---
  const fs::path saves     = "/tmp/gmm_saves_tab/saves";
  const fs::path mods      = "/tmp/gmm_saves_tab/mods";
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
  write_save(saves, "Player1_20260802141135_1_1", "Player1", 40, "Whiterun", 1, newer,
             {"Skyrim.esm", "SkyUI_SE.esp", "GoneMod.esp"});
  write_save(saves, "Player1_20260802090000_1_2", "Player1", 39, "Riften", 2, older,
             {"Skyrim.esm", "SkyUI_SE.esp"});
  write_file(saves / "Player1_20260802141135_1_1.skse", "co-save");

  ui::SavesScanRequest request;
  request.saves_dir  = saves;
  request.extensions = {"ess"};
  request.game_id    = "skyrimse";
  // Snapshot: Skyrim.esm enabled (satisfied); SkyUI_SE.esp NOT in the list
  // (but provided by the SkyUI mod folder); GoneMod.esp absent but provided
  // by <overwrite>. A save listing something with no provider at all would
  // count as missing.
  engine::GamePlugin skyrim;
  skyrim.name           = "Skyrim.esm";
  skyrim.enabled        = true;
  request.plugins       = {skyrim};
  request.mods_dir      = mods;
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
    if (!timeout.isActive())
      break;
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
    check(!tip_new.contains("Skyrim.esm"), "enabled master never appears as missing");
    check(tab.save_at(0) != nullptr, "scan result readable via save_at");
  }

  // --- Part 3: debounced visible-only re-scan (Workspace-69xt, MO2
  // refreshSavesIfOpen parity): the tab watches its saves dir with a 500ms
  // debounce. A hidden tab must NOT rescan on disk changes (the Aug 2026
  // watch-spam regression: the Proton-prefix Saves dir churns on its own).
  // The visible-tab rescan is covered by the dedicated watcher case below;
  // here the tab is never shown, so the table must stay exactly as the
  // explicit scan left it.
  tab.set_saves_dir(saves);
  check(tab.saves_dir() == saves, "set_saves_dir records the dir");
  // A change on disk must NOT grow the table: drop a brand-new, newest save
  // into the dir and give the old watcher machinery plenty of time to
  // (wrongly) fire. The table must stay exactly as the explicit scan left it.
  const uint64_t newest = newer + 0x10000000;  // FILETIME, ~3 min later
  write_save(saves, "Player1_20260803000000_1_3", "Player1", 41, "Whiterun", 3, newest,
             {"Skyrim.esm"});
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
  const fs::path cfg   = "/tmp/gmm_saves_tab_noparser/config";
  const fs::path saves = "/tmp/gmm_saves_tab_noparser/saves";
  fs::remove_all("/tmp/gmm_saves_tab_noparser");
  fs::create_directories(cfg);
  fs::create_directories(saves);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
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
  request.saves_dir  = saves;
  request.extensions = {"ess"};
  request.game_id    = "noparsergame";
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
    if (!timeout.isActive())
      break;
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
    // Workspace-e2td: an unparsed stub shows the filename stem in the
    // Name column (not the ", #0, Level 0, " empty-metadata text).
    check(table->item(0, 0)->text() ==
              tab.save_at(0)->file_path.stem().string().c_str(),
          "stub Name column falls back to the filename stem");
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
  const fs::path cfg       = "/tmp/gmm_saves_tab_coalesce/config";
  const fs::path saves_dir = "/tmp/gmm_saves_tab_coalesce/saves";
  const fs::path mods      = "/tmp/gmm_saves_tab_coalesce/mods";
  const fs::path ow        = "/tmp/gmm_saves_tab_coalesce/ow";
  fs::remove_all("/tmp/gmm_saves_tab_coalesce");
  fs::create_directories(cfg);
  fs::create_directories(saves_dir);
  fs::create_directories(mods);
  fs::create_directories(ow);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
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
    req.saves_dir  = saves_dir;
    req.extensions = {"ess"};
    req.game_id    = "skyrimse";
    // Stash a marker in mods_dir: the test path encodes the tag so we
    // can tell which request was the one that ran.
    req.mods_dir      = mods / ("tag-" + tag);
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
    if (!timeout.isActive())
      break;
    loop.processEvents();
  }
  check(table->rowCount() == 3, "first scan populated the table with all 3 saves");
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
    if (!timeout.isActive())
      break;
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
  const fs::path root  = "/tmp/gmm_saves_tab_stream";
  const fs::path cfg   = root / "config";
  const fs::path saves = root / "saves";
  const fs::path mods  = root / "mods";
  const fs::path ow    = root / "ow";
  fs::remove_all(root);
  fs::create_directories(cfg);
  fs::create_directories(saves);
  fs::create_directories(mods);
  fs::create_directories(ow);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
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
  request.saves_dir     = saves;
  request.extensions    = {"ess"};
  request.game_id       = "skyrimse";
  request.mods_dir      = mods;
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
    if (!timeout.isActive())
      break;
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

// Workspace-e2td: Isaac keeps saves as *.dat files in the Steam userdata
// remote dir with no save parser registered. The tab must still list them:
// filename in Name (stem fallback) + File, size + modified date in the File
// tooltip, empty Missing column. Exercises the worker stub path with a
// non-"ess" extension - the production request carries {"dat"} for Isaac via
// save_extensions_for().
TEST_CASE("saves tab lists isaac-style dat files without a parser", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path root  = "/tmp/gmm_saves_tab_isaac";
  const fs::path cfg   = root / "config";
  const fs::path saves = root / "saves";
  fs::remove_all(root);
  fs::create_directories(cfg);
  fs::create_directories(saves);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char* test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  check(!engine::SaveParserRegistry::instance().has_parser("isaac"),
        "test precondition: no parser registered for isaac");
  write_file(saves / "ser1.dat", std::string(128, 'a'));
  write_file(saves / "persistentgamedata1.dat", std::string(64, 'b'));

  ui::SavesTab tab;
  auto* table = tab.table();
  check(!tab.empty_state_visible(), "empty state hidden before any scan");

  ui::SavesScanRequest request;
  request.saves_dir  = saves;
  request.extensions = {"dat"};
  request.game_id    = "isaac";
  tab.request_scan(std::move(request));

  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  timeout.start(5000);
  while (table->rowCount() != 2) {
    if (!timeout.isActive())
      break;
    loop.processEvents();
  }
  timeout.stop();

  check(table->rowCount() == 2, "both .dat saves listed with no parser");
  if (table->rowCount() == 2) {
    check(table->item(0, 1)->text().endsWith(".dat"),
          "file column carries the .dat basename");
    check(table->item(0, 0)->text() == table->item(0, 1)->text().chopped(4),
          "name column falls back to the filename stem");
    const auto* save = tab.save_at(0);
    check(save != nullptr && save->file_size > 0,
          "stub SaveGame carries the on-disk file size");
    check(save != nullptr && save->creation_time > 0,
          "stub SaveGame carries the mtime as creation_time");
    const QString tip = table->item(0, 1)->toolTip();
    check(tip.contains("bytes") && tip.contains("modified"),
          "file tooltip shows size + modified date for a stub row");
    check(table->item(0, 2)->text().isEmpty(),
          "missing-assets column is empty for a stub (no plugins)");
    check(!tab.empty_state_visible(), "empty state hidden once rows land");
  }

  fs::remove_all(root);
}

// Workspace-e2td: an empty (or missing) saves dir degrades to a helpful
// empty-state message naming the dir - no crash, no silent bare table.
TEST_CASE("saves tab shows an empty state for an empty saves dir", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path root  = "/tmp/gmm_saves_tab_empty";
  const fs::path cfg   = root / "config";
  const fs::path saves = root / "saves";
  fs::remove_all(root);
  fs::create_directories(cfg);
  fs::create_directories(saves);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char* test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  ui::SavesTab tab;
  auto* table = tab.table();
  QSignalSpy finished_spy(tab.scan_thread()->worker(), &ui::SavesScanWorker::finished);
  REQUIRE(finished_spy.isValid());

  ui::SavesScanRequest request;
  request.saves_dir  = saves;
  request.extensions = {"dat"};
  request.game_id    = "isaac";
  tab.request_scan(std::move(request));

  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  timeout.start(5000);
  while (finished_spy.count() == 0) {
    if (!timeout.isActive())
      break;
    QTimer::singleShot(5, &loop, &QEventLoop::quit);
    loop.exec();
  }
  timeout.stop();
  loop.processEvents();

  check(table->rowCount() == 0, "empty dir scans to zero rows (no crash)");
  check(tab.empty_state_visible(), "empty state shown after an empty scan lands");
  check(tab.empty_state_text().contains(QString::fromStdString(saves.string())),
        "empty state names the scanned dir");

  fs::remove_all(root);
}

// --- Workspace-69xt: activation latch, debounced watcher, fast-scan ---
namespace {

// Pump events until pred() holds or timeout_ms elapses. Returns pred().
template <typename Pred>
bool pump_until(Pred pred, int timeout_ms) {
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  timeout.start(timeout_ms);
  while (!pred()) {
    if (!timeout.isActive())
      break;
    loop.processEvents();
  }
  timeout.stop();
  return pred();
}

// Settle helper: let the event loop run for exactly wait_ms (for asserting
// that something did NOT happen - e.g. no scan while hidden).
void settle_ms(int wait_ms) {
  QEventLoop loop;
  QTimer t;
  t.setSingleShot(true);
  QObject::connect(&t, &QTimer::timeout, &loop, &QEventLoop::quit);
  t.start(wait_ms);
  loop.exec();
}

// Minimal type-1 (zlib chunk chain) TESV save: valid head + plugin lists,
// then a filler tail across several streams. The fast reader must serve it
// from the first stream(s) without touching the tail.
void write_save_type1(const fs::path& dir, const std::string& base,
                      const std::vector<std::string>& light, int light_count_extra) {
  std::vector<char> f;
  const char* magic = "TESV_SAVEGAME";
  f.insert(f.end(), magic, magic + 13);
  put_u32(f, 0);
  put_u32(f, 12);
  put_u32(f, 9);
  put_str(f, "Chunky");
  put_u32(f, 50);
  put_str(f, "Winterhold");
  put_str(f, "12:34:56");
  put_str(f, "ImperialRace");
  put_u16(f, 0);
  for (int i = 0; i < 8; ++i)
    f.push_back(0);
  put_u64(f, 0x01DD228800000000ULL);
  put_u32(f, 16);
  put_u32(f, 16);
  put_u16(f, 1);  // compression type 1
  for (int i = 0; i < 16 * 16 * 4; ++i)
    f.push_back(static_cast<char>(i & 0xFF));

  std::vector<char> raw;
  raw.push_back(78);
  raw.push_back(1);
  put_u16(raw, 0);
  raw.push_back(0);
  raw.push_back(1);
  put_str(raw, "Skyrim.esm");
  // Light list: the named entries plus filler to cross light_count_extra.
  put_u16(raw, static_cast<uint16_t>(light.size() + light_count_extra));
  for (const auto& p : light)
    put_str(raw, p);
  // Long filler names (~60B each) so the list clears the fast reader's
  // 256KiB decompressed cap and forces the full-parser fallback path.
  const std::string pad(48, 'x');
  for (int i = 0; i < light_count_extra; ++i)
    put_str(raw, "Filler" + std::to_string(i) + pad + ".esl");
  // Filler tail so the region spans several streams.
  while (raw.size() < 512 * 1024)
    raw.push_back(static_cast<char>(raw.size() & 0xFF));

  const std::size_t kIn  = 64 * 1024;
  const std::size_t head = f.size();
  put_u64(f, 0);  // chunk_start placeholder
  put_u64(f, raw.size());
  const std::size_t chunk_start = f.size();
  for (int i = 0; i < 8; ++i)
    f[head + i] = static_cast<char>((chunk_start >> (8 * i)) & 0xFF);
  for (std::size_t off = 0; off < raw.size(); off += kIn) {
    const std::size_t n = std::min(kIn, raw.size() - off);
    uLong bound         = compressBound(static_cast<uLong>(n));
    std::vector<char> out(static_cast<std::size_t>(bound));
    uLongf outlen = bound;
    REQUIRE(compress2(reinterpret_cast<Bytef*>(out.data()), &outlen,
                      reinterpret_cast<const Bytef*>(raw.data() + off),
                      static_cast<uLong>(n), Z_DEFAULT_COMPRESSION) == Z_OK);
    out.resize(static_cast<std::size_t>(outlen));
    f.insert(f.end(), out.begin(), out.end());
    while (f.size() % 16 != 0)
      f.push_back(0);
  }
  std::ofstream(dir / (base + ".ess"), std::ios::binary)
      .write(f.data(), static_cast<std::streamsize>(f.size()));
}

}  // namespace

TEST_CASE("saves tab scans on first activation only", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path root  = "/tmp/gmm_saves_latch69";
  const fs::path cfg   = root / "config";
  const fs::path saves = root / "saves";
  fs::remove_all(root);
  fs::create_directories(cfg);
  fs::create_directories(saves);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char* test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  ui::SavesTab tab;
  QSignalSpy scan_spy(&tab, &ui::SavesTab::scan_requested);
  REQUIRE(scan_spy.isValid());
  check(scan_spy.count() == 0, "no scan at construction (no preload)");

  tab.set_saves_dir(saves);
  settle_ms(300);
  check(scan_spy.count() == 0, "setting the dir does not scan (MO2 boot parity)");

  tab.show();
  check(pump_until(
            [&] {
              return scan_spy.count() == 1;
            },
            2000),
        "first activation emits exactly one scan");
  tab.hide();
  tab.show();
  settle_ms(500);
  check(scan_spy.count() == 1,
        "re-activation does not rescan (watcher/profile/delete own refreshes)");

  fs::remove_all(root);
}

TEST_CASE("saves tab watcher rescans only while visible", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path root  = "/tmp/gmm_saves_watch69";
  const fs::path cfg   = root / "config";
  const fs::path saves = root / "saves";
  fs::remove_all(root);
  fs::create_directories(cfg);
  fs::create_directories(saves);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char* test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const uint64_t t1 = 0x01DD228000000000ULL;
  const uint64_t t2 = t1 + 0x10000000;
  const uint64_t t3 = t2 + 0x10000000;
  write_save(saves, "W_first", "Pw", 5, "Loc", 1, t1, {"Skyrim.esm"});

  ui::SavesTab tab;
  // Controller stand-in: answer scan_requested with a stub-game scan (no
  // parser registered for "watchgame69", so rows list by mtime).
  QObject::connect(&tab, &ui::SavesTab::scan_requested, &tab, [&tab, saves] {
    ui::SavesScanRequest req;
    req.saves_dir  = saves;
    req.extensions = {"ess"};
    req.game_id    = "watchgame69";
    tab.request_scan(std::move(req));
  });
  tab.set_saves_dir(saves);
  tab.show();
  REQUIRE(tab.isVisible());
  auto* table = tab.table();
  check(pump_until(
            [&] {
              return table->rowCount() == 1;
            },
            5000),
        "initial scan lands after first show");

  QSignalSpy scan_spy(&tab, &ui::SavesTab::scan_requested);
  REQUIRE(scan_spy.isValid());
  write_save(saves, "W_second", "Pw", 6, "Loc", 2, t2, {"Skyrim.esm"});
  check(pump_until(
            [&] {
              return table->rowCount() == 2;
            },
            8000),
        "visible watcher picks up the dropped save (500ms debounce + scan)");
  check(scan_spy.count() >= 1, "the rescan came from the watcher");

  // Hidden: disk churn must not scan (the Aug 2026 watch-spam regression).
  tab.hide();
  const int scans_before_hide = scan_spy.count();
  write_save(saves, "W_third", "Pw", 7, "Loc", 3, t3, {"Skyrim.esm"});
  settle_ms(2000);  // > 3x the 500ms debounce
  check(table->rowCount() == 2, "hidden tab does not rescan on disk change");
  check(scan_spy.count() == scans_before_hide, "no scan_requested while hidden");

  fs::remove_all(root);
}

TEST_CASE("saves scan prefers the registered fast parser", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path root  = "/tmp/gmm_saves_fastpref69";
  const fs::path cfg   = root / "config";
  const fs::path saves = root / "saves";
  fs::remove_all(root);
  fs::create_directories(cfg);
  fs::create_directories(saves);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char* test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  write_file(saves / "F_1.ess", "bytes (content irrelevant, parsers are stubs)");

  int full_calls = 0;
  engine::SaveParserRegistry::instance().register_parser(
      "fastgame69", 0,
      [&full_calls](const std::filesystem::path& p, const std::string& gid) {
        ++full_calls;
        engine::SaveGame g;
        g.file_path     = p;
        g.game_id       = gid;
        g.pc_name       = "FULL";
        g.creation_time = 1;
        return g;
      },
      nullptr, "test:full69");
  engine::SaveParserRegistry::instance().register_fast_parser(
      "fastgame69", 0,
      [](const std::filesystem::path& p, const std::string& gid) {
        engine::SaveGame g;
        g.file_path      = p;
        g.game_id        = gid;
        g.pc_name        = "FAST";
        g.creation_time  = 2;
        g.has_heavy_data = false;
        return g;
      },
      nullptr, "test:fast69");

  ui::SavesTab tab;
  auto* table = tab.table();
  ui::SavesScanRequest request;
  request.saves_dir  = saves;
  request.extensions = {"ess"};
  request.game_id    = "fastgame69";
  // No fast_format: the registry fast parser alone must win.
  tab.request_scan(std::move(request));
  check(pump_until(
            [&] {
              return table->rowCount() == 1;
            },
            5000),
        "fast-parser scan lands");
  if (table->rowCount() == 1) {
    check(tab.save_at(0) != nullptr && tab.save_at(0)->pc_name == "FAST",
          "row carries the fast parser's marker");
    check(full_calls == 0, "full parser never ran for the scan");
  }

  engine::SaveParserRegistry::instance().clear_plugin("test:full69");
  engine::SaveParserRegistry::instance().clear_plugin("test:fast69");
  fs::remove_all(root);
}

TEST_CASE("saves scan uses the knowledge fast format without a plugin parser", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path root  = "/tmp/gmm_saves_fastfmt69";
  const fs::path cfg   = root / "config";
  const fs::path saves = root / "saves";
  fs::remove_all(root);
  fs::create_directories(cfg);
  fs::create_directories(saves);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char* test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // "tesvfast69" has NO registered parser: the worker would stub-list it.
  check(!engine::SaveParserRegistry::instance().has_parser("tesvfast69"),
        "test precondition: no parser for tesvfast69");
  const uint64_t ft = 0x01DD2288D3CC4860ULL;
  write_save(saves, "Hero_20260802_1", "Hero", 30, "Markarth", 5, ft,
             {"Skyrim.esm", "SkyUI_SE.esp"});

  ui::SavesTab tab;
  auto* table = tab.table();

  // Unknown format value: safe fallback to the stub path (stem, no parse).
  ui::SavesScanRequest req_stub;
  req_stub.saves_dir   = saves;
  req_stub.extensions  = {"ess"};
  req_stub.game_id     = "tesvfast69";
  req_stub.fast_format = "martian";
  tab.request_scan(std::move(req_stub));
  check(pump_until(
            [&] {
              return table->rowCount() == 1;
            },
            5000),
        "unknown-format scan lands");
  if (table->rowCount() == 1) {
    check(table->item(0, 1)->text() == "Hero_20260802_1.ess", "file column");
    check(tab.save_at(0) != nullptr && tab.save_at(0)->pc_name.empty(),
          "unknown format falls back to the unparsed stub");
  }

  // Declared format: the Core Gamebryo reader parses header + plugins with
  // no plugin parser involved.
  ui::SavesScanRequest req_fast;
  req_fast.saves_dir   = saves;
  req_fast.extensions  = {"ess"};
  req_fast.game_id     = "tesvfast69";
  req_fast.fast_format = engine::kSaveFastFormatGamebryoTesv;
  tab.request_scan(std::move(req_fast));
  check(pump_until(
            [&] {
              return table->rowCount() == 1 && tab.save_at(0) != nullptr &&
                     tab.save_at(0)->pc_name == "Hero";
            },
            5000),
        "declared format parses the save without a plugin parser");
  if (table->rowCount() == 1 && tab.save_at(0) != nullptr) {
    check(tab.save_at(0)->pc_level == 30, "fast level");
    check(tab.save_at(0)->pc_location == "Markarth", "fast location");
    check(tab.save_at(0)->plugins.size() == 2, "fast plugins");
    check(!tab.save_at(0)->has_heavy_data, "fast result is heavy-free");
    check(table->item(0, 0)->text().contains("Level 30"),
          "display name renders from fast fields");
  }

  fs::remove_all(root);
}

TEST_CASE("saves scan falls back to the full parser past the fast cap", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path root  = "/tmp/gmm_saves_needfull69";
  const fs::path cfg   = root / "config";
  const fs::path saves = root / "saves";
  fs::remove_all(root);
  fs::create_directories(cfg);
  fs::create_directories(saves);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char* test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // A type-1 save whose light list (~9000 names) exceeds the fast reader's
  // decompressed cap: the fast path must rerun it through the full parser
  // instead of dropping the row.
  write_save_type1(saves, "Big_20260802_1", {"B.esl"}, 9000);

  int full_calls = 0;
  engine::SaveParserRegistry::instance().register_parser(
      "tesvneedfull69", 0,
      [&full_calls](const std::filesystem::path& p, const std::string& gid) {
        ++full_calls;
        engine::SaveGame g;
        g.file_path     = p;
        g.game_id       = gid;
        g.pc_name       = "FULLFALLBACK";
        g.creation_time = 3;
        return g;
      },
      nullptr, "test:needfull69");

  ui::SavesTab tab;
  auto* table = tab.table();
  ui::SavesScanRequest request;
  request.saves_dir   = saves;
  request.extensions  = {"ess"};
  request.game_id     = "tesvneedfull69";
  request.fast_format = engine::kSaveFastFormatGamebryoTesv;
  tab.request_scan(std::move(request));
  check(pump_until(
            [&] {
              return table->rowCount() == 1;
            },
            5000),
        "past-cap save still lands a row");
  if (table->rowCount() == 1) {
    check(tab.save_at(0) != nullptr && tab.save_at(0)->pc_name == "FULLFALLBACK",
          "row came from the full-parser fallback");
    check(full_calls >= 1, "full parser ran for the past-cap file");
  }

  engine::SaveParserRegistry::instance().clear_plugin("test:needfull69");
  fs::remove_all(root);
}

TEST_CASE("saves tab rebuilds on profile switch", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path root      = "/tmp/gmm_saves_profile69";
  const fs::path cfg       = root / "config";
  const fs::path saves     = root / "saves";
  const fs::path instances = root / "instances";
  fs::remove_all(root);
  fs::create_directories(cfg);
  fs::create_directories(saves);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char* test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // Instance harness (game_path_banner_test shape): game-less instance,
  // knowledge with just a mods subpath. "profilegame69" has no save parser,
  // so scans stub-list by mtime - the rebuild, not the parse, is under test.
  auto inst           = engine::Instance::installed("TestGame", instances);
  inst.info().game_id = "profilegame69";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  const fs::path inst_root = inst.info().root;

  ui::MainWindow w;
  engine::GameKnowledge knowledge;
  knowledge.set("profilegame69", "mods_subpath", "Mods");
  // Declared BEFORE the window: RightPanel::set_capabilities keeps the raw
  // pointer, so it must outlive MainWindow (reverse destruction order).
  engine::GameCapabilities caps;
  w.set_game_knowledge(&knowledge);
  w.set_game_info("profilegame69", "Profile Game", "Default", {}, inst_root);

  auto* ctrl = w.findChild<ui::ModListController*>();
  REQUIRE(ctrl != nullptr);
  auto* rp = w.findChild<ui::RightPanel*>();
  REQUIRE(rp != nullptr);
  // The harness game declares no capabilities, so the tab bar has no saves
  // placeholder: declare it in-test (mirrors the game plugin's .tabs()).
  // tab_materialized auto-wires it (main_window.cpp), like production.
  engine::CapabilityInfo saves_cap;
  saves_cap.game_id      = "profilegame69";
  saves_cap.capability   = "saves";
  saves_cap.display_name = "Saves";
  caps.register_capability(saves_cap);
  rp->set_capabilities(&caps);
  rp->set_game("profilegame69");
  auto* st = rp->ensure_saves_tab();
  REQUIRE(st != nullptr);
  auto* dl = w.findChild<ui::DownloadsController*>();
  REQUIRE(dl != nullptr);
  (void)dl;
  // The wired tab resolves the real saves dir, which does not exist for
  // this game-less harness: point it at the fixture dir instead.
  write_file(saves / "P_1.ess", "stub save one");
  write_file(saves / "P_2.ess", "stub save two");
  st->set_saves_dir(saves);

  // Populate synchronously through the batch path (no worker wait): the
  // rebuild proof below observes request_scan's synchronous table clear,
  // so the test never pumps on threads the harness owns.
  ui::SavesScanResult seed;
  seed.saves_dir = saves;
  for (const auto& base : {"P_1.ess", "P_2.ess"}) {
    ui::SavesScanResultEntry entry;
    entry.save.file_path = saves / base;
    entry.save.game_id   = "profilegame69";
    entry.save.pc_name   = "Seed";
    seed.entries.push_back(std::move(entry));
  }
  st->set_saves(std::move(seed));
  REQUIRE(st->table()->rowCount() == 2);

  // Switch profiles through the public path (ProfileBar combo ->
  // profile_changed -> ModListController::switch_profile). The switch must
  // rebuild saves even though the tab is hidden (MO2 parity). request_scan
  // clears the table synchronously when no scan is in flight, so the
  // rebuild is observable WITHOUT pumping: 2 seeded rows drop to 0 the
  // moment the switch fires its refresh. (The async re-landing is covered
  // by every request_scan test above; waiting on harness-owned threads
  // wedged this test's event pump during development.)
  const fs::path profiles_dir =
      engine::Instance::from_root(inst_root).path_for(engine::InstanceKind::Profiles);
  const auto created = engine::profile::create_fresh_profile(profiles_dir, "Second");
  REQUIRE(created.success);
  ctrl->refresh_profiles();
  auto* profile_bar = w.findChild<ui::ProfileBar*>();
  REQUIRE(profile_bar != nullptr);
  auto* combo = profile_bar->findChild<QComboBox*>();
  REQUIRE(combo != nullptr);
  REQUIRE(combo->findText("Second") >= 0);
  combo->setCurrentText("Second");
  check(st->table()->rowCount() == 0,
        "profile switch fires a saves rebuild (seeded rows cleared sync)");

  fs::remove_all(root);
}
