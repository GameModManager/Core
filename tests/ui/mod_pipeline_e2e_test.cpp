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
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMouseEvent>
#include <QPushButton>
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

  // The downloads entry reached Installed and is still on screen with "Hide
  // installed" ON.
  CHECK(cell_text(table, row_v1, ui::DownloadsTab::Status) == "Installed");
  CHECK_FALSE(table->isRowHidden(row_v1));
  // The sibling is untouched - it is a different file, genuinely not installed.
  CHECK(cell_text(table, row_v2, ui::DownloadsTab::Status) == "Install");

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
  CHECK_FALSE(table->isRowHidden(row_v2));
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

  // The one production step the HTTP reply's body would hand to.
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

  // What is NOT proven here, stated rather than papered over: the HTTP GET
  // itself. Everything from the parsed body onward is the real code, and
  // everything before it is the real install, but no key is reachable on this
  // box and the network answers 401, so `from_nexus` is handed to
  // apply_installed_version directly rather than read out of a socket. The
  // byte transfer is the one seam - the same gap step 7 already declares for
  // the update check, and it is deliberately NOT closed by a second invented
  // capture, which would only ever test this test.

  fs::remove_all(root);
}