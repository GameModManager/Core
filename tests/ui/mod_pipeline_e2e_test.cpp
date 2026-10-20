// The mod pipeline, end to end, in one pass:
//
//   download -> install -> verify -> newer version arrives -> download it ->
//   install over the top -> verify again -> the update check notices.
//
// Everything in here is the real thing. Real MainWindow, real Downloads table,
// real mod list, real zip archives built with libarchive, real PipelineWorker
// running the real ExtractStage/InstallStage on its own thread, and the two
// modal decisions a real overwrite install puts in front of the user answered
// by clicking the real buttons. Nothing is stubbed, nothing is called by hand
// to make a step pass.
//
// What is NOT here, and why:
//
//   - The byte transfer. A download needs a Nexus API key; no key is reachable
//     on this box (no nexus.key, nothing in config, secret-tool empty, the
//     network answers 401). The two archives are therefore staged the way a
//     finished download restores - the archive on disk plus its own
//     "<archive>.meta" - which is byte-for-byte the state
//     DownloadsTab/load_download_manifest present on the next launch. Every
//     step after the bytes exist is the real path.
//   - Step 7's live round trip, for the same reason. It is proven as far as the
//     parser plus the real captured fixture and the honest no-key gate, and no
//     further. That gap is stated in the report, not papered over.
//
// The per-step fixes this covers were each shipped and each proven separately
// (install_stage.cpp Replace deleting meta.ini, add_installed_mod refreshing
// the row, focus_installed_mod, mark_installed vs the installed filter, the
// files.json shape). The point of this file is the loop: every one of those
// fixes is a single step, and the user has been finding the next broken one
// after the last one was merged.
#include "engine/game/registry/game_knowledge.h"
#include "engine/instance/instance.h"
#include "engine/mod/meta/mod_meta.h"
#include "engine/source/nexus/update_check.h"
#include "test_platform.h"
#include "ui/controllers/downloads_controller.h"
#include "ui/controllers/mod_list_controller.h"
#include "ui/install/install_name_dialog.h"
#include "ui/main_window/main_window.h"
#include "ui/overwrite/query_overwrite_dialog.h"
#include "ui/panels/downloads_tab.h"
#include "ui/settings/settings.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"
#include "ui/widgets/right_panel.h"

#include <archive.h>
#include <archive_entry.h>

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QTabBar>
#include <QTableWidget>
#include <QTabWidget>
#include <QTest>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path &p, const std::string &content) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p);
  out << content;
}

std::string read_text(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

struct ZipItem {
  std::string path;
  std::string content;
};

// A real zip through libarchive's writer, so ExtractStage reads a real archive
// rather than a stand-in (the pattern filetree_test.cpp established).
bool make_zip(const fs::path &zip_path, const std::vector<ZipItem> &items) {
  struct archive *a = archive_write_new();
  archive_write_set_format_zip(a);
  if (archive_write_open_filename(a, zip_path.c_str()) != ARCHIVE_OK) {
    archive_write_free(a);
    return false;
  }
  bool ok = true;
  for (const auto &item : items) {
    struct archive_entry *e = archive_entry_new();
    archive_entry_set_pathname(e, item.path.c_str());
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, 0644);
    archive_entry_set_size(e, static_cast<la_int64_t>(item.content.size()));
    if (archive_write_header(a, e) != ARCHIVE_OK) {
      ok = false;
    } else if (!item.content.empty() &&
               archive_write_data(a, item.content.data(), item.content.size()) < 0) {
      ok = false;
    }
    archive_entry_free(e);
    if (!ok)
      break;
  }
  archive_write_close(a);
  archive_write_free(a);
  return ok;
}

// Polls a predicate while pumping the event loop; false on timeout so the case
// FAILs with context instead of hanging the suite.
template <typename Fn>
bool pump_until(Fn pred, int timeout_ms = 30000) {
  QElapsedTimer timer;
  timer.start();
  while (!pred()) {
    if (timer.elapsed() > timeout_ms)
      return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return true;
}

// Answers the modals the real install puts up, by clicking the real buttons.
// Without this the install hangs inside exec() offscreen.
//
// An unrecognised modal is CLOSED rather than left up: it unwinds as Cancel, so
// the case FAILs on the missing install with the dialog's title in the message
// instead of sitting there until the ctest timeout.
struct ModalAnswerer {
  QTimer poll;
  std::string install_name_button = "Install";
  std::string overwrite_button    = "Replace";
  QString refused_modal;

  static QPushButton *button_named(QWidget *root, const std::string &text) {
    for (auto *b : root->findChildren<QPushButton *>()) {
      if (b->text().toStdString() == text)
        return b;
    }
    return nullptr;
  }

  void arm() {
    poll.setInterval(25);
    QObject::connect(&poll, &QTimer::timeout, [this] {
      auto *modal = QApplication::activeModalWidget();
      if (!modal)
        return;
      if (auto *name = qobject_cast<ui::InstallNameDialog *>(modal)) {
        if (auto *b = button_named(name, install_name_button)) {
          b->click();
          return;
        }
      }
      if (auto *ow = qobject_cast<ui::QueryOverwriteDialog *>(modal)) {
        if (auto *b = button_named(ow, overwrite_button)) {
          b->click();
          return;
        }
      }
      if (refused_modal.isEmpty())
        refused_modal = modal->metaObject()->className();
      modal->close();
    });
    poll.start();
  }

  void disarm() { poll.stop(); }
};

// Every mod folder needs a meta.ini before the window loads, or the scanner has
// nothing to read a name, a version or a position from.
void seed_mod(const fs::path &mods_dir, const std::string &folder,
              const std::string &version) {
  write_file(mods_dir / folder / (folder + ".esp"), "plugin");
  write_file(mods_dir / folder / "meta.ini", "[General]\nversion=" + version +
                                                 "\ninstalled=2026-01-01T00:00:00\n"
                                                 "[GameModManager]\nfolder=" +
                                                 folder + "\n");
}

int find_mod_row(ui::ModView *view, const QString &id) {
  const auto *m = view->model();
  for (int r = 0; r < m->rowCount(); ++r) {
    for (int c = 0; c < m->columnCount(); ++c) {
      if (m->index(r, c).data().toString() == id)
        return r;
    }
  }
  return -1;
}

// Two downloads of the same mod share a display name, so the row cannot be
// found by name - the archive in the row's tooltip is what identifies it.
int find_download_row_for_archive(QTableWidget *table, const QString &archive) {
  for (int r = 0; r < table->rowCount(); ++r) {
    auto *name = table->item(r, ui::DownloadsTab::Name);
    if (name && name->toolTip().contains(archive))
      return r;
  }
  return -1;
}

std::string cell_text(QTableWidget *table, int row, int col) {
  auto *item = table->item(row, col);
  return item ? item->text().toStdString() : std::string("<no cell>");
}

// The user's own gesture on a downloads row: a synthesized platform
// double-click sequence (Press, Release, DblClick, Release). QTest::mouseDClick
// does not produce cellDoubleClicked on these tables offscreen - the same
// reason main_window_harness_test.cpp sends the four events by hand.
void double_click_cell(QTableWidget *table, int row, int col) {
  const QRect cell = table->visualItemRect(table->item(row, col));
  REQUIRE(cell.isValid());
  const QPoint pos    = cell.center();
  const QPoint global = table->viewport()->mapToGlobal(pos);
  QMouseEvent press(QEvent::MouseButtonPress, pos, global, Qt::LeftButton,
                    Qt::LeftButton, Qt::NoModifier);
  QMouseEvent release(QEvent::MouseButtonRelease, pos, global, Qt::LeftButton,
                      Qt::NoButton, Qt::NoModifier);
  QMouseEvent dblpress(QEvent::MouseButtonDblClick, pos, global, Qt::LeftButton,
                       Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(table->viewport(), &press);
  QCoreApplication::sendEvent(table->viewport(), &release);
  QCoreApplication::sendEvent(table->viewport(), &dblpress);
  QCoreApplication::sendEvent(table->viewport(), &release);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

// A tab switch by a real click on the tab bar, so the page under test is
// actually current and its widgets are visible (a click into a hidden page is
// silently dropped offscreen).
void click_tab(QTabWidget *tabs, const QString &label) {
  auto *bar = tabs->findChild<QTabBar *>();
  REQUIRE(bar != nullptr);
  for (int i = 0; i < bar->count(); ++i) {
    if (bar->tabText(i) != label)
      continue;
    QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(i).center());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return;
  }
  FAIL("no tab named " << label.toStdString());
}

// A mod's Version cell as the user reads it.
std::string version_cell(ui::ModList *model, const QString &id) {
  const int row = model->priority_of(id);
  if (row < 0)
    return "<no row>";
  return model->index(row, ui::ModList::Version).data().toString().toStdString();
}

// Whether the row actually ADVERTISES an available update. The verdict in
// meta.ini is the input; this is the output the user sees - the Version cell's
// tooltip, built by the model out of the same shows_update() the badge icon is
// gated on. The tooltip rather than the icon on purpose, matching the existing
// harness case: icon packs are staged beside the binary and are not populated
// under ctest, so whether the artwork paints is the user's GUI check. The
// decision the badge is gated on is not.
bool wears_update_badge(ui::ModList *model, const QString &id) {
  const int row = model->priority_of(id);
  if (row < 0)
    return false;
  return model->index(row, ui::ModList::Version)
      .data(Qt::ToolTipRole)
      .toString()
      .contains(QStringLiteral("Newest version"));
}

// Whether the Version cell is painted BOLD AND RED, the way MO2 paints a mod
// that has a newer file waiting.
//
// Unlike the badge, this needs no icon pack and no GUI check: it reads the two
// roles MO2's own delegate reads off the model, so it is the model's data
// contract rather than a pixel. MO2 answers all three for COL_VERSION and the
// icon is only one of them - ModList::data returns
//   FontRole      setWeight(QFont::Bold) when updateAvailable() (modlist.cpp:335)
//   ForegroundRole QBrush(Qt::red) on the same predicate (modlist.cpp:358)
//   DecorationRole the update_available icon (modlist.cpp:346)
// so replacing the icon alone dropped two thirds of what MO2 renders, and the
// user saw the missing part. Dropping either branch below reds this out.
bool wears_bold_red_version(ui::ModList *model, const QString &id) {
  const int row = model->priority_of(id);
  if (row < 0)
    return false;
  const QModelIndex cell = model->index(row, ui::ModList::Version);
  const auto font        = cell.data(Qt::FontRole).value<QFont>();
  const auto brush       = cell.data(Qt::ForegroundRole).value<QBrush>();
  return font.weight() == QFont::Bold && brush.color() == QColor(Qt::red);
}

}  // namespace

TEST_CASE(
    "Mod pipeline: download, install, update, reinstall keeps position and version",
    "[ui][harness][pipeline][e2e]") {
  // Per-run throwaway root under /tmp/opencode. HOME and BOTH XDG vars are
  // redirected: QSettings falls back to $HOME whenever XDG_CONFIG_HOME is
  // unset, and trash_root() falls back to $HOME/.local/share/Trash, so a
  // partial redirect leaves the real user's config and trash reachable.
  const fs::path root = fs::path("/tmp/opencode") / "gmm_pipeline_e2e";
  fs::remove_all(root);
  fs::create_directories(root / "config");
  fs::create_directories(root / "data");
  fs::create_directories(root / "instances");
  qputenv("XDG_CONFIG_HOME", QByteArray((root / "config").string().c_str()));
  qputenv("XDG_DATA_HOME", QByteArray((root / "data").string().c_str()));
  qputenv("HOME", QByteArray(root.string().c_str()));

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "mod_pipeline_e2e_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  auto &settings = Settings::instance();
  // set_game_info posts an NXM-handler prompt; "Don't show" is a real user
  // choice and its early-out keeps a modal from appearing inside the first
  // processEvents offscreen.
  settings.set_nxm_handler_check("dont_ask");
  // The post-install update poll is a network call on its own thread; the
  // update-check half of this test is driven explicitly and offline below.
  settings.set_check_update_after_mod_install(false);
  // The setting the user has ON. Left on for the whole run: an install that
  // the filter takes away in the same breath it records is one of the failures
  // this file exists to catch.
  settings.set_hide_installed_downloads(true);

  auto inst           = engine::Instance::installed("TestGame", root / "instances");
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  const fs::path inst_root = inst.info().root;
  const auto instance      = engine::Instance::from_root(inst_root);
  const fs::path mods_dir  = instance.path_for(engine::InstanceKind::Mods);
  const fs::path dl_dir    = instance.path_for(engine::InstanceKind::Downloads);
  fs::create_directories(dl_dir);

  // Two mods the install must land BETWEEN, so "keeps its position" is a claim
  // about a middle row and not about the bottom of the list.
  seed_mod(mods_dir, "Aaa", "1.0.0");
  seed_mod(mods_dir, "Zzz", "1.0.0");

  // --- the archives -------------------------------------------------------
  // v1 ships a file that v2 does not, so the overwrite can be shown to have
  // REPLACED the folder rather than merged into it.
  const std::string meta_v1 =
      "[General]\nversion=1.0.0\ninstallationFile=Foo-1.0.0.zip\n"
      "[GameModManager]\nfolder=Foo\n";
  REQUIRE(make_zip(dl_dir / "Foo-1.0.0.zip",
                   {{"Data/Foo.esp", "plugin v1"},
                    {"Data/OnlyInV1.txt", "gone after the update"},
                    {"meta.ini", meta_v1}}));
  REQUIRE(make_zip(
      dl_dir / "Foo-2.0.0.zip",
      {{"Data/Foo.esp", "plugin v2"},
       {"Data/AddedInV2.txt", "new in the update"},
       {"meta.ini", "[General]\nversion=2.0.0\ninstallationFile=Foo-2.0.0.zip\n"
                    "[GameModManager]\nfolder=Foo\n"}}));

  // The state a finished download restores from: the archive on disk plus its
  // own "<archive>.meta". Both rows read "Foo" because that is what two
  // downloads of one mod look like.
  struct Row {
    const char *archive;
    const char *id;
    int file_id;
  };
  const Row rows[] = {
      {"Foo-1.0.0.zip", "6832-16268", 16268},
      {"Foo-2.0.0.zip", "6832-17000", 17000},
  };
  for (const auto &r : rows) {
    const fs::path archive = dl_dir / r.archive;
    ui::DownloadMeta meta;
    meta.archive       = archive;
    meta.id            = r.id;
    meta.name          = "Foo";
    meta.source        = "Nexus Mods";
    meta.parent_mod_id = "6832";
    meta.file_id       = r.file_id;
    meta.nexus_domain  = "testgame";
    meta.total_size    = static_cast<int64_t>(fs::file_size(archive));
    meta.state         = ui::DownloadState::Complete;
    REQUIRE(ui::write_download_meta(meta, ui::download_meta_path(meta, dl_dir)));
  }

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *panel = w.findChild<ui::RightPanel *>();
  REQUIRE(panel != nullptr);
  auto *tab = panel->ensure_downloads_tab();
  REQUIRE(tab != nullptr);
  auto *table = tab->table();
  REQUIRE(table != nullptr);
  // Switch the panel onto Downloads with a real click: the install gesture is
  // delivered to the table, so the page has to be the current one.
  click_tab(panel->tab_widget(), QStringLiteral("Downloads"));

  auto *model = w.findChild<ui::ModList *>();
  auto *ctl   = w.findChild<ui::ModListController *>();
  auto *view  = w.mod_view();
  REQUIRE(model != nullptr);
  REQUIRE(ctl != nullptr);
  REQUIRE(view != nullptr);
  REQUIRE(w.pipeline_worker() != nullptr);
  REQUIRE(pump_until([&] {
    return find_mod_row(view, QStringLiteral("Zzz")) >= 0;
  }));

  // === STEP 1: the finished downloads are in the Downloads tab =============
  const int row_v1 = find_download_row_for_archive(table, QStringLiteral("Foo-1.0.0"));
  const int row_v2 = find_download_row_for_archive(table, QStringLiteral("Foo-2.0.0"));
  REQUIRE(row_v1 >= 0);
  REQUIRE(row_v2 >= 0);
  REQUIRE(row_v1 != row_v2);
  INFO("download rows: v1=" << row_v1 << " v2=" << row_v2);
  CHECK(cell_text(table, row_v1, ui::DownloadsTab::Status) == "Install");
  CHECK(cell_text(table, row_v2, ui::DownloadsTab::Status) == "Install");

  ModalAnswerer modals;
  modals.arm();

  // === STEP 2/3/4/5: install the first archive ============================
  // The user's own gesture: double-click the row.
  double_click_cell(table, row_v1, ui::DownloadsTab::Name);

  REQUIRE(pump_until([&] {
    return fs::exists(mods_dir / "Foo" / "meta.ini");
  }));
  REQUIRE(pump_until([&] {
    return cell_text(table, row_v1, ui::DownloadsTab::Status) == "Installed";
  }));
  REQUIRE(modals.refused_modal.isEmpty());

  // Files on disk, from the archive.
  REQUIRE(fs::exists(mods_dir / "Foo" / "Data" / "Foo.esp"));
  CHECK(read_text(mods_dir / "Foo" / "Data" / "Foo.esp") == "plugin v1");
  CHECK(fs::exists(mods_dir / "Foo" / "Data" / "OnlyInV1.txt"));

  // meta.ini exists and is ours, not just copied through.
  auto meta = engine::ModMeta::load(mods_dir, "Foo");
  CHECK(meta.get("GameModManager", "folder") == "Foo");
  CHECK(meta.get("General", "installationFile") == "Foo-1.0.0.zip");
  // The download's own identity is recorded, which is what the update check
  // later compares against.
  CHECK(meta.get("Nexusmods", "modid") == "6832");
  CHECK(meta.get("Nexusmods", "fileid") == "16268");

  // A mod list row, carrying the REAL version out of the archive - not "?" and
  // not a fabricated "1.0".
  REQUIRE(pump_until([&] {
    return find_mod_row(view, QStringLiteral("Foo")) >= 0;
  }));
  CHECK(version_cell(model, QStringLiteral("Foo")) == "1.0.0");

  // The downloads entry reached Installed, and a ticked "Hide installed" - MO2's
  // "Hide downloads after installation" - took it off the screen in the same
  // call. The cell still says Installed: the row is hidden, not deleted.
  CHECK(cell_text(table, row_v1, ui::DownloadsTab::Status) == "Installed");
  CHECK(table->isRowHidden(row_v1));
  // The sibling is untouched - it is a different file, genuinely not installed.
  CHECK(cell_text(table, row_v2, ui::DownloadsTab::Status) == "Install");
  CHECK_FALSE(table->isRowHidden(row_v2));

  // === STEP 5: focused ====================================================
  const int foo_row_v1 = model->priority_of(QStringLiteral("Foo"));
  REQUIRE(foo_row_v1 >= 0);
  REQUIRE(pump_until([&] {
    return view->currentIndex().row() == foo_row_v1;
  }));
  CHECK(view->currentIndex().row() == foo_row_v1);

  // Put Foo inside a separator band, between the two seeded mods, and persist
  // that through the app's own sync calls - a separator_id and a priority that
  // only exist in the model would not survive the folder being replaced.
  const QString sep_id =
      ctl->create_separator_named(QStringLiteral("Group"), QString());
  REQUIRE_FALSE(sep_id.isEmpty());
  model->move_mod(sep_id, 1);
  model->move_mod(QStringLiteral("Foo"), 2);
  ctl->sync_priorities();
  ctl->sync_separator_ids();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

  const int foo_row_before     = model->priority_of(QStringLiteral("Foo"));
  const std::string sep_before = engine::ModMeta::load(mods_dir, "Foo").separator_id();
  REQUIRE(foo_row_before > 0);
  REQUIRE_FALSE(sep_before.empty());
  INFO("Foo before the update: row=" << foo_row_before << " separator=" << sep_before);

  // === the update symbol Foo must LOSE during that update ==================
  // The user's second report from the same run: the flag was still there after
  // installing the newest version. The sweep's verdict is seeded on the v1
  // install exactly as UpdateCheck::run() writes it - the newest file is 2.0.0,
  // which is the file the user is about to install over the top.
  {
    auto flagged = engine::ModMeta::load(mods_dir, "Foo");
    flagged.set("General", "nexus_update", "true");
    flagged.set("General", "newestversion", "2.0.0");
    REQUIRE(flagged.save(mods_dir, "Foo"));
  }
  // Load the list so the ROW wears it, rather than trusting that the file on
  // disk is enough. The assertions after the install are about the row changing.
  ctl->load_mods_from_game();
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));
  REQUIRE(pump_until([&] {
    return find_mod_row(view, QStringLiteral("Foo")) >= 0;
  }));
  // The precondition: the badge really is on the row before the install path
  // runs, so a green assertion after it is measuring a change.
  CHECK(wears_update_badge(model, QStringLiteral("Foo")));
  CHECK(version_cell(model, QStringLiteral("Foo")) == "1.0.0");
  // MO2 paints that same cell bold red, not just badged. The user's third
  // report from this run was "we show an orange icon, MO2 shows bold red
  // text", so the look is pinned where the update verdict genuinely exists -
  // a row that really has a newer file waiting, not a synthesised one.
  CHECK(wears_bold_red_version(model, QStringLiteral("Foo")));

  // === STEP 6: install the newer archive OVER the top =====================
  modals.overwrite_button = "Replace";
  double_click_cell(table, row_v2, ui::DownloadsTab::Name);

  REQUIRE(pump_until([&] {
    return fs::exists(mods_dir / "Foo" / "Data" / "AddedInV2.txt");
  }));
  REQUIRE(pump_until([&] {
    return cell_text(table, row_v2, ui::DownloadsTab::Status) == "Installed";
  }));
  REQUIRE(modals.refused_modal.isEmpty());

  // The superseded download. This is the user's own screenshot: two real
  // downloads of one mod, and the OLD one sitting at "Install" forever, which is
  // the action that would put 1.0.0 back on top of the 2.0.0 they just
  // installed. The Replace branch reports the archive it replaced
  // (ctx.replaced_archive, out of the old meta.ini's installationfile), and MO2
  // routes exactly that through modReplaced -> markUninstalled
  // (installationmanager.cpp:423, mainwindow.cpp:2555). So it reads
  // "Uninstalled", and it is persisted that way in its own meta.
  REQUIRE(pump_until([&] {
    return cell_text(table, row_v1, ui::DownloadsTab::Status) == "Uninstalled";
  }));
  CHECK(cell_text(table, row_v1, ui::DownloadsTab::Status) != "Install");
  {
    QSettings superseded_meta(
        QString::fromStdString((dl_dir / "Foo-1.0.0.zip.meta").string()),
        QSettings::IniFormat);
    CHECK(superseded_meta.value("uninstalled").toBool());
    // BOTH flags, which is MO2's own shape and the reason the derivation order
    // matters: its markInstalled clears "uninstalled" and its markUninstalled
    // leaves "installed" alone (downloadmanager.cpp:1549-1552, :1588-1591), so a
    // superseded archive's meta carries installed=true AND uninstalled=true, and
    // only reading uninstalled first puts it back on screen as "Uninstalled".
    CHECK(superseded_meta.value("installed").toBool());
  }
  // With the filter on, NEITHER row for this mod is on screen: both are
  // no-longer-pending states, and one of them reading "Install" is exactly the
  // thing the user is meant not to see after installing over the top.
  CHECK(table->isRowHidden(row_v1));
  CHECK(table->isRowHidden(row_v2));

  // Files replaced, not merged: v1's own file is gone, v2's content is in place.
  CHECK_FALSE(fs::exists(mods_dir / "Foo" / "Data" / "OnlyInV1.txt"));
  REQUIRE(fs::exists(mods_dir / "Foo" / "Data" / "Foo.esp"));
  CHECK(read_text(mods_dir / "Foo" / "Data" / "Foo.esp") == "plugin v2");

  // One mod folder, ONE row for it: an overwrite install must not append a
  // second copy of the mod. (Row-count arithmetic is the wrong proxy for that
  // - the list also carries the permanent __overwrite__ pseudo-row, which is
  // a feature and not a duplicate - so this counts the rows that are Foo.)
  {
    int foo_rows = 0;
    std::string ids;
    for (const auto &m : model->mods()) {
      ids += (ids.empty() ? "" : ",") + m.id.toStdString();
      if (m.id == QLatin1String("Foo"))
        ++foo_rows;
    }
    INFO("model rows: " << ids);
    CHECK(foo_rows == 1);
  }
  CHECK(find_mod_row(view, QStringLiteral("Foo")) >= 0);

  // Position kept, in the session AND on disk. The meta.ini keys are what a
  // reload reads, so both are asserted: the in-session row number, and the
  // priority + separator_id the folder has to carry for the next launch.
  CHECK(model->priority_of(QStringLiteral("Foo")) == foo_row_before);
  const auto after_meta = engine::ModMeta::load(mods_dir, "Foo");
  CHECK(after_meta.priority() >= 0);
  CHECK(after_meta.separator_id() == sep_before);
  CHECK(after_meta.get("Nexusmods", "fileid") == "17000");
  CHECK(after_meta.get("General", "installationFile") == "Foo-2.0.0.zip");

  // === the update symbol, cleared by the REAL path ========================
  // This is the leg that closes the uovb gap, so read it before trusting it.
  // Nothing below calls reconcile_update_symbol, apply_installed_version or
  // record_installed_version. The install above went through the user's own
  // double-click, the real PipelineWorker emitted install_complete, and the
  // real DownloadsController lambda ran - which is where
  // record_installed_version lives. There is no Nexus API key on this box, and
  // there does not have to be one: the reconcile compares two strings already
  // in meta.ini and spends no request, so it runs BEFORE the key gate
  // (downloads_controller.cpp). Gating it behind the key is what let the flag
  // outlive the update in the first place.
  //
  // Two production call sites have to be live for this to pass, and each one
  // alone leaves it red:
  //   1. install_stage.cpp's carry of nexus_update / newestversion across the
  //      Replace. Without it write_game_metadata's fresh five-key meta.ini has
  //      no flag for the reconcile to read and it returns without acting.
  //   2. the reconcile call in record_installed_version. Delete it and the row
  //      keeps the badge.
  // That is what makes this control different from the direct call it replaces,
  // where both of those deletions left the suite green.
  CHECK(after_meta.get("General", "version") == "2.0.0");
  CHECK(after_meta.get("General", "nexus_update") == "false");
  // CONSUMER-REACHES-CONTROL: the verdict on disk is not the claim - the row no
  // longer wearing the symbol is.
  CHECK_FALSE(wears_update_badge(model, QStringLiteral("Foo")));
  // ...and the bold red goes with it. Clearing the flag has to take the
  // painting with it, or a current mod sits there shouting about an update
  // nobody has: set_update_state repaints FontRole and ForegroundRole for
  // exactly this reason, and this is the assertion that the repaint list is
  // not missing either of them.
  CHECK_FALSE(wears_bold_red_version(model, QStringLiteral("Foo")));

  // The version that made the verdict stale is still the newest the sweep named,
  // so the row can still say what it is at. The clear is not a wipe.
  CHECK(after_meta.get("General", "newestversion") == "2.0.0");

  // Reload from disk: the position and the band are read back from meta.ini,
  // which is the only place they can come from after a restart.
  ctl->load_mods_from_game();
  REQUIRE(pump_until([&] {
    return find_mod_row(view, QStringLiteral("Foo")) >= 0;
  }));
  CHECK(model->priority_of(QStringLiteral("Foo")) == foo_row_before);
  CHECK(engine::ModMeta::load(mods_dir, "Foo").separator_id() == sep_before);

  // Version updated, downloads row correct, focus correct.
  CHECK(version_cell(model, QStringLiteral("Foo")) == "2.0.0");
  CHECK(cell_text(table, row_v2, ui::DownloadsTab::Status) == "Installed");
  CHECK(table->isRowHidden(row_v2));
  // Still Uninstalled after the reload: nothing re-derived it back to
  // "Install".
  CHECK(cell_text(table, row_v1, ui::DownloadsTab::Status) == "Uninstalled");
  CHECK(table->isRowHidden(row_v1));
  REQUIRE(pump_until([&] {
    return view->currentIndex().isValid() &&
           view->currentIndex().row() == model->priority_of(QStringLiteral("Foo"));
  }));

  modals.disarm();

  // === STEP 7: the update check ===========================================
  // Proven as far as it honestly can be without a key: the REAL captured Nexus
  // body for this mod through the real parser, the verdict the sweep would
  // reach, and the gate that makes a keyless check say so instead of claiming
  // "up to date". The live round trip is NOT proven here.
  namespace UC           = engine::Source::Nexus::UpdateCheck;
  const fs::path fixture = fs::path(__FILE__).parent_path().parent_path() / "engine" /
                           "fixtures" / "nexus" / "files_6832.json";
  std::ifstream in(fixture, std::ios::binary);
  REQUIRE(in.good());
  const std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  REQUIRE(!body.empty());

  std::vector<long long> available;
  std::string newest, installed_version;
  // The file id the install just recorded (17000) is a newer upload than the
  // capture holds, so the capture cannot state ITS version - and the parser
  // must say "not stated" instead of blanking the recorded one.
  REQUIRE(UC::parse_files(body, 17000, available, newest, installed_version));
  REQUIRE(available.size() == 1);
  CHECK(available.front() == 16268);
  CHECK(newest == "v1.0");
  CHECK(installed_version.empty());

  // The capture's own file: our file is the newest Nexus serves. This is the
  // "0 updates from 64 queried" regression answered from real data.
  CHECK(UC::evaluate(16268, available).state == UC::Verdict::Current);
  // A mod we know nothing about is never reported as up to date.
  CHECK(UC::evaluate(0, available).state == UC::Verdict::Unknown);
  // A file id older than the newest on offer is the update verdict itself.
  CHECK(UC::evaluate(100, available).state == UC::Verdict::Update);
  CHECK(UC::update_visible(true, "2.1.0", "") == true);

  // The gate a keyless user actually hits: the sweep reports the missing key
  // and writes nothing. It must not stamp a version or claim a clean sweep.
  const auto report = UC::run("testgame", "", mods_dir, 1789000000, true);
  CHECK_FALSE(report.ok);
  CHECK(report.updates == 0);
  CHECK(report.requests == 0);
  CHECK(report.message.find("API key") != std::string::npos);
  // Nothing was fabricated into the mod the sweep could not answer for.
  CHECK(engine::ModMeta::load(mods_dir, "Foo").get("General", "version") == "2.0.0");

  fs::remove_all(root);
}

// A second leg, on a game whose metadata file IS meta.ini - which is every
// MO2-style game, Skyrim included. It exists because the leg above cannot fail
// for the reason the user's own case fails.
//
// Foo-1.0.0.zip SHIPS a populated meta.ini, and for testgame (and for Skyrim)
// ctx.metadata_file IS "meta.ini". Two separate places make that archive's own
// version win without anything ever reading it as metadata:
//
//   extract_stage.cpp:158 gates the metadata parse on
//   `ctx.metadata_file != "meta.ini"`, so mod.version stays EMPTY for an MO2
//   game, and
//   mod_meta.cpp:806 write_game_metadata early-returns when meta.ini already
//   exists, so the version the archive SHIPPED is carried across by the plain
//   file copy and never passes through the parse at all.
//
// So step 3 passes on a fixture that has nothing to do with the gate the user
// hits. This archive ships NO meta.ini, which is what a Nexus Skyrim archive
// looks like: nothing to copy, nothing to gate, nothing to parse. The only
// thing that can put a version in the row is Nexus answering files.json - and
// the archive NAME says 1.24.0 while the real captured body says "v1.0" for
// the same file, so a filename-guessing or embedded-meta implementation fails
// here by construction.
TEST_CASE("Mod pipeline: an MO2-game archive that ships no version takes it from Nexus",
          "[ui][harness][pipeline][e2e][version]") {
  const fs::path root = fs::path("/tmp/opencode") / "gmm_pipeline_e2e_version";
  fs::remove_all(root);
  fs::create_directories(root / "config");
  fs::create_directories(root / "data");
  fs::create_directories(root / "instances");
  qputenv("XDG_CONFIG_HOME", QByteArray((root / "config").string().c_str()));
  qputenv("XDG_DATA_HOME", QByteArray((root / "data").string().c_str()));
  qputenv("HOME", QByteArray(root.string().c_str()));

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "mod_pipeline_e2e_version_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  auto &settings = Settings::instance();
  settings.set_nxm_handler_check("dont_ask");
  // Off, so the post-install update poll cannot race this case for the mods
  // dir or stamp a version of its own over what is being asserted here.
  settings.set_check_update_after_mod_install(false);
  settings.set_hide_installed_downloads(true);

  auto inst           = engine::Instance::installed("TestGame", root / "instances");
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  const fs::path inst_root = inst.info().root;
  const auto instance      = engine::Instance::from_root(inst_root);
  const fs::path mods_dir  = instance.path_for(engine::InstanceKind::Mods);
  const fs::path dl_dir    = instance.path_for(engine::InstanceKind::Downloads);
  fs::create_directories(dl_dir);

  // The archive a Nexus Skyrim download actually looks like: a Data/ tree
  // holding the plugin, a readme at the TOP level beside it (the shape MO2's
  // getSimpleArchiveBase accepts - one data dir plus junk files - and the
  // shape Nexus really ships), and NO meta.ini. The name carries 1.24.0
  // because that is what Nexus puts in a filename, and a filename is not a
  // contract.
  const char *archive_name = "CrashLogger-59818-1.24.0.zip";
  REQUIRE(make_zip(dl_dir / archive_name, {{"Data/CrashLogger.esp", "plugin"},
                                           {"CrashLogger.txt", "readme"}}));

  ui::DownloadMeta meta;
  meta.archive       = dl_dir / archive_name;
  meta.id            = "59818-16268";
  meta.name          = "CrashLogger";
  meta.source        = "Nexus Mods";
  meta.parent_mod_id = "59818";
  meta.file_id       = 16268;
  meta.nexus_domain  = "testgame";
  meta.total_size    = static_cast<int64_t>(fs::file_size(dl_dir / archive_name));
  meta.state         = ui::DownloadState::Complete;
  REQUIRE(ui::write_download_meta(meta, ui::download_meta_path(meta, dl_dir)));

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *panel = w.findChild<ui::RightPanel *>();
  REQUIRE(panel != nullptr);
  auto *tab = panel->ensure_downloads_tab();
  REQUIRE(tab != nullptr);
  auto *table = tab->table();
  REQUIRE(table != nullptr);
  click_tab(panel->tab_widget(), QStringLiteral("Downloads"));

  auto *model = w.findChild<ui::ModList *>();
  auto *view  = w.mod_view();
  REQUIRE(model != nullptr);
  REQUIRE(view != nullptr);

  const int row = find_download_row_for_archive(table, QStringLiteral("CrashLogger"));
  REQUIRE(row >= 0);
  CHECK(cell_text(table, row, ui::DownloadsTab::Status) == "Install");

  ModalAnswerer modals;
  modals.arm();

  // === the real path: the user's own double-click =========================
  double_click_cell(table, row, ui::DownloadsTab::Name);

  REQUIRE(pump_until([&] {
    return fs::exists(mods_dir / "CrashLogger" / "meta.ini");
  }));
  REQUIRE(pump_until([&] {
    return cell_text(table, row, ui::DownloadsTab::Status) == "Installed";
  }));
  REQUIRE(modals.refused_modal.isEmpty());
  modals.disarm();

  // Files on disk, from the archive. The Data/ wrapper is merged into the mod
  // root (staging_layout's merged_data_dir - the one data dir plus a top-level
  // readme, which is what MO2's getSimpleArchiveBase folds), so the plugin
  // lands beside the readme rather than under Data/.
  REQUIRE(fs::exists(mods_dir / "CrashLogger" / "CrashLogger.esp"));
  CHECK(read_text(mods_dir / "CrashLogger" / "CrashLogger.esp") == "plugin");
  CHECK(fs::exists(mods_dir / "CrashLogger" / "CrashLogger.txt"));

  REQUIRE(pump_until([&] {
    return find_mod_row(view, QStringLiteral("CrashLogger")) >= 0;
  }));

  // === what the install ACTUALLY left: no version at all ==================
  // The archive shipped none, and for an MO2 game extract_stage.cpp:158 will
  // not read one even if it had. This is the assertion the first leg cannot
  // make - it is the difference between "the version arrived" and "a version
  // arrived".
  const auto after_install = engine::ModMeta::load(mods_dir, "CrashLogger");
  CHECK(after_install.get("General", "version").empty());
  // Nor did the archive's NAME become the version. This is the discriminator:
  // a filename-guessing implementation fails exactly here and nowhere else.
  CHECK(version_cell(model, QStringLiteral("CrashLogger")) != "1.24.0");
  // Nor the "1.0" an older install used to fabricate out of nothing.
  CHECK(version_cell(model, QStringLiteral("CrashLogger")) != "1.0");

  // The two gates record_installed_version evaluates before it spends a
  // request, read off the state the REAL install wrote. The empty version
  // above IS the first gate - downloads_controller.cpp:1308 skips when a
  // version is already on file - and these two are the second, the bail at
  // :1315 for a missing mod id or file id. All three open is the only state
  // in which the fetch happens at all; closed, nothing ever asks Nexus and the
  // version never lands, which is the bug this leg exists to keep shut.
  CHECK(after_install.get("Nexusmods", "modid") == "59818");
  REQUIRE(after_install.get("Nexusmods", "fileid") == "16268");
  REQUIRE(std::stoll(after_install.get("Nexusmods", "fileid")) == 16268);

  // === the version Nexus answers with, not the one the filename suggests ===
  // The REAL captured v1 files.json reply for this mod, through the REAL
  // parser, for the file id the install just recorded.
  const fs::path fixture = fs::path(__FILE__).parent_path().parent_path() / "engine" /
                           "fixtures" / "nexus" / "files_6832.json";
  std::ifstream in(fixture, std::ios::binary);
  REQUIRE(in.good());
  const std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  REQUIRE(!body.empty());

  namespace UC = engine::Source::Nexus::UpdateCheck;
  std::vector<long long> available;
  std::string newest, from_nexus;
  REQUIRE(UC::parse_files(body, 16268, available, newest, from_nexus));
  REQUIRE(from_nexus == "v1.0");

  // The update symbol has to agree with the version, and it is seeded BEFORE
  // the step below so the step below is what clears it.
  //
  // The user's own report: the Version column reads 1.0.2 and the row is STILL
  // wearing the update-available symbol. The sweep had recorded
  // nexus_update=true with newestversion=1.0.2; the install then recorded
  // version=1.0.2 - the same string - and nothing re-read the verdict, so the
  // badge outlived the fact that produced it. The badge is not cleared by
  // waiting for the next sweep either; run() is monthly by default.
  {
    auto *ctl = w.findChild<ui::ModListController *>();
    REQUIRE(ctl != nullptr);
    auto flagged = engine::ModMeta::load(mods_dir, "CrashLogger");
    INFO("CrashLogger meta: " << flagged.serialize());
    flagged.set("General", "nexus_update", "true");
    flagged.set("General", "newestversion", from_nexus);
    REQUIRE(flagged.save(mods_dir, "CrashLogger"));
    ctl->load_mods_from_game();
    // Wait for the SCAN, not for the row. The scan runs on ModScanThread and
    // load_mods_from_game is what the mod is badged by; the row was already on
    // screen from the load above, so waiting for it proves the pump ran and
    // says nothing about whether the new meta has been read. `loading_` goes
    // false after the per-mod loop that calls set_update_state, which is the
    // call under test's precondition.
    REQUIRE(pump_until([&w] {
      return !w.is_loading();
    }));
    REQUIRE(pump_until([&] {
      return find_mod_row(view, QStringLiteral("CrashLogger")) >= 0;
    }));
    // The badge really is worn before the version-recording step touches
    // anything, so the assertions after it are measuring a change and not a row
    // that never had one.
    CHECK(wears_update_badge(model, QStringLiteral("CrashLogger")));
  }

  // The one production step the HTTP reply's body would hand to. This is the
  // seam this leg declares and no more: no key is reachable on this box and the
  // network answers 401, so `from_nexus` is handed to the real function rather
  // than read out of a socket. Everything else here - the write into meta.ini,
  // the row repaint and the update symbol being re-read - runs inside it.
  ui::DownloadsController::apply_installed_version(&w, mods_dir, "CrashLogger",
                                                   from_nexus);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

  // === the claim ==========================================================
  CHECK(engine::ModMeta::load(mods_dir, "CrashLogger").get("General", "version") ==
        "v1.0");
  CHECK(version_cell(model, QStringLiteral("CrashLogger")) == "v1.0");
  // It is NOT the version the archive name would have suggested, and NOT the
  // "1.0" an install used to fabricate, and it is no longer "?" either.
  CHECK(version_cell(model, QStringLiteral("CrashLogger")) != "1.24.0");
  CHECK(version_cell(model, QStringLiteral("CrashLogger")) != "1.0");
  CHECK(version_cell(model, QStringLiteral("CrashLogger")) != "?");
  // The flag itself, written by the reconcile at the end of that same call -
  // remove it and this goes red, which is what the direct-call version of this
  // check could not do.
  CHECK(engine::ModMeta::load(mods_dir, "CrashLogger").get("General", "nexus_update") ==
        "false");
  // CONSUMER-REACHES-CONTROL: the verdict on disk is not the claim, the row no
  // longer wearing the symbol is.
  CHECK_FALSE(wears_update_badge(model, QStringLiteral("CrashLogger")));

  // What is NOT proven here, stated rather than papered over: the HTTP GET
  // itself. Everything from the parsed body onward is the real code, and
  // everything before it is the real install, but no key is reachable on this
  // box and the network answers 401, so `from_nexus` is handed to
  // apply_installed_version directly rather than read out of a socket. The
  // byte transfer is the one seam - the same gap step 7 already declares for
  // the update check, and it is deliberately NOT closed by a second invented
  // capture, which would only ever test this test.

  // A version that is NOT the newest leaves the symbol alone - the reconcile is
  // not a blanket "the install cleared it". Driven through the same production
  // call site, so this is a claim about that function and not about the test.
  //
  // The reload is what puts the re-flagged verdict back ON THE ROW. Without it
  // the row still holds the verdict the clear above wrote and this check would
  // be measuring the test's own bookkeeping rather than the reconcile - the
  // meta on disk would be the only honest thing to assert here, and that is not
  // what the user sees.
  {
    auto *ctl = w.findChild<ui::ModListController *>();
    REQUIRE(ctl != nullptr);
    auto older = engine::ModMeta::load(mods_dir, "CrashLogger");
    older.set("General", "nexus_update", "true");
    REQUIRE(older.save(mods_dir, "CrashLogger"));
    ctl->load_mods_from_game();
    REQUIRE(pump_until([&w] {
      return !w.is_loading();
    }));
    REQUIRE(pump_until([&] {
      return find_mod_row(view, QStringLiteral("CrashLogger")) >= 0;
    }));
    REQUIRE(wears_update_badge(model, QStringLiteral("CrashLogger")));

    ui::DownloadsController::apply_installed_version(&w, mods_dir, "CrashLogger",
                                                     "0.9");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    CHECK(
        engine::ModMeta::load(mods_dir, "CrashLogger").get("General", "nexus_update") ==
        "true");
    // 0.9 is not the newest the sweep named, so an update really IS still
    // waiting and the row has to keep saying so.
    CHECK(wears_update_badge(model, QStringLiteral("CrashLogger")));
  }

  fs::remove_all(root);
}

// The LoversLab verdict, end to end through the REAL load path and the REAL
// install path. The sweep itself is covered engine-side
// (page_source_sweep_test.cpp); what only a UI test can prove is that a
// verdict written under [General] update_available - the page-scrape sources'
// key, not Nexus's - reaches the row: the load path must read it for a
// loverslab-sourced mod, and the install's reconcile must clear it through the
// same gate. Both used to be Nexus-only, so a green engine test would have
// proven nothing about what the user sees.
TEST_CASE("Mod pipeline: a LoversLab verdict badges the row and the install clears it",
          "[ui][harness][pipeline][e2e][loverslab]") {
  const fs::path root = fs::path("/tmp/opencode") / "gmm_pipeline_e2e_loverslab";
  fs::remove_all(root);
  fs::create_directories(root / "config");
  fs::create_directories(root / "data");
  fs::create_directories(root / "instances");
  qputenv("XDG_CONFIG_HOME", QByteArray((root / "config").string().c_str()));
  qputenv("XDG_DATA_HOME", QByteArray((root / "data").string().c_str()));
  qputenv("HOME", QByteArray(root.string().c_str()));

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "mod_pipeline_e2e_loverslab_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  auto &settings = Settings::instance();
  settings.set_nxm_handler_check("dont_ask");
  settings.set_check_update_after_mod_install(false);
  settings.set_hide_installed_downloads(true);

  auto inst           = engine::Instance::installed("TestGame", root / "instances");
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  const fs::path inst_root = inst.info().root;
  const auto instance      = engine::Instance::from_root(inst_root);
  const fs::path mods_dir  = instance.path_for(engine::InstanceKind::Mods);
  const fs::path dl_dir    = instance.path_for(engine::InstanceKind::Downloads);
  fs::create_directories(dl_dir);

  // The LoversLab mod, seeded exactly as the sweep leaves it after finding an
  // update: the verdict under [General] update_available, the newest version
  // beside it, the page provenance in [LoversLab].
  write_file(mods_dir / "LlpMod" / "LlpMod.esp", "plugin v1");
  write_file(mods_dir / "LlpMod" / "meta.ini",
             "[General]\n"
             "version=1.0.0\n"
             "update_available=true\n"
             "newestversion=2.0.0\n"
             "[GameModManager]\n"
             "folder=LlpMod\n"
             "source_type=loverslab\n"
             "[LoversLab]\n"
             "fileid=6832\n"
             "page_url=https://www.loverslab.com/files/file/6832-example/\n");

  // The newer archive the user is about to install over it. Its own meta.ini
  // carries the LoversLab provenance, so the installed folder stays
  // loverslab-sourced and the reconcile reads the right verdict key.
  const std::string meta_v2 =
      "[General]\nversion=2.0.0\ninstallationFile=LlpMod-2.0.0.zip\n"
      "[GameModManager]\nfolder=LlpMod\nsource_type=loverslab\n"
      "[LoversLab]\nfileid=6832\n"
      "page_url=https://www.loverslab.com/files/file/6832-example/\n";
  REQUIRE(make_zip(dl_dir / "LlpMod-2.0.0.zip",
                   {{"Data/LlpMod.esp", "plugin v2"}, {"meta.ini", meta_v2}}));

  ui::DownloadMeta meta;
  meta.archive    = dl_dir / "LlpMod-2.0.0.zip";
  meta.id         = "ll-6832";
  meta.name       = "LlpMod";
  meta.source     = "LoversLab";
  meta.total_size = static_cast<int64_t>(fs::file_size(dl_dir / "LlpMod-2.0.0.zip"));
  meta.state      = ui::DownloadState::Complete;
  REQUIRE(ui::write_download_meta(meta, ui::download_meta_path(meta, dl_dir)));

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *panel = w.findChild<ui::RightPanel *>();
  REQUIRE(panel != nullptr);
  auto *tab = panel->ensure_downloads_tab();
  REQUIRE(tab != nullptr);
  auto *table = tab->table();
  REQUIRE(table != nullptr);
  click_tab(panel->tab_widget(), QStringLiteral("Downloads"));

  auto *model = w.findChild<ui::ModList *>();
  auto *ctl   = w.findChild<ui::ModListController *>();
  auto *view  = w.mod_view();
  REQUIRE(model != nullptr);
  REQUIRE(ctl != nullptr);
  REQUIRE(view != nullptr);
  REQUIRE(pump_until([&] {
    return find_mod_row(view, QStringLiteral("LlpMod")) >= 0;
  }));

  // === the verdict reaches the row ========================================
  // The load path read [General] update_available for a loverslab-sourced mod
  // and the row is wearing it - bold red, the same rendering the Nexus verdict
  // gets, because the user has one idea of "an update is waiting".
  CHECK(wears_update_badge(model, QStringLiteral("LlpMod")));
  CHECK(version_cell(model, QStringLiteral("LlpMod")) == "1.0.0");
  CHECK(wears_bold_red_version(model, QStringLiteral("LlpMod")));

  // NEGATIVE CONTROL: flip the verdict off on disk, reload, and the row must
  // stop wearing it. Without this a green check above could be the load path
  // badgeing everything.
  {
    auto cleared = engine::ModMeta::load(mods_dir, "LlpMod");
    cleared.set("General", "update_available", "false");
    REQUIRE(cleared.save(mods_dir, "LlpMod"));
    ctl->load_mods_from_game();
    REQUIRE(pump_until([&w] {
      return !w.is_loading();
    }));
    REQUIRE(pump_until([&] {
      return find_mod_row(view, QStringLiteral("LlpMod")) >= 0;
    }));
    CHECK_FALSE(wears_update_badge(model, QStringLiteral("LlpMod")));

    // And back on, for the install below to clear.
    auto flagged = engine::ModMeta::load(mods_dir, "LlpMod");
    flagged.set("General", "update_available", "true");
    REQUIRE(flagged.save(mods_dir, "LlpMod"));
    ctl->load_mods_from_game();
    REQUIRE(pump_until([&w] {
      return !w.is_loading();
    }));
    REQUIRE(pump_until([&] {
      return find_mod_row(view, QStringLiteral("LlpMod")) >= 0;
    }));
    REQUIRE(wears_update_badge(model, QStringLiteral("LlpMod")));
  }

  // === the real install clears it =========================================
  const int row = find_download_row_for_archive(table, QStringLiteral("LlpMod-2.0.0"));
  REQUIRE(row >= 0);
  ModalAnswerer modals;
  modals.arm();
  double_click_cell(table, row, ui::DownloadsTab::Name);

  REQUIRE(pump_until([&] {
    return fs::exists(mods_dir / "LlpMod" / "Data" / "LlpMod.esp");
  }));
  REQUIRE(pump_until([&] {
    return cell_text(table, row, ui::DownloadsTab::Status) == "Installed";
  }));
  REQUIRE(modals.refused_modal.isEmpty());
  modals.disarm();

  // The install recorded the archive's own version, which IS the newest the
  // verdict named - so the reconcile's gate (installed == newest) fires and
  // the verdict is cleared on disk...
  CHECK(engine::ModMeta::load(mods_dir, "LlpMod").get("General", "version") == "2.0.0");
  CHECK(engine::ModMeta::load(mods_dir, "LlpMod").get("General", "update_available") ==
        "false");
  // ...and CONSUMER-REACHES-CONTROL: the row stops wearing it. This is the
  // stale-badge bug the reconcile exists for, on the page-scrape sources' key.
  CHECK_FALSE(wears_update_badge(model, QStringLiteral("LlpMod")));

  fs::remove_all(root);
}