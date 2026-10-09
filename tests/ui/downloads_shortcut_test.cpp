// Tests for the two Downloads-tab capabilities this build added on top of
// MO2's download list:
//
//   1. Enter / Delete / Space on the selected row (MO2
//      DownloadListView::keyPressEvent, downloadlistview.cpp:330-360). The
//      state -> key mapping is exercised directly through the free function
//      download_shortcut_for, and the routing is exercised through a real
//      QKeyEvent delivered to the table's event filter, so a mapping that is
//      right but never wired (or wired to the wrong signal) fails.
//
//   2. The Name cell's hover text (MO2's ToolTipRole branch,
//      downloadlist.cpp:213-232). This build has no Nexus description to show
//      - the Version column was dropped for that reason - so the tooltip
//      carries the on-disk path and the source page instead, which the
//      columns never show.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, no network.
#include "ui/panels/tab_panels.h"

#include <QApplication>
#include <QKeyEvent>
#include <QTableWidget>
#include <QTableWidgetItem>

#include "ui/settings/settings.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

// One QApplication per test case, as the other UI tests do: Catch runs cases
// in one process, so a single shared instance would have to outlive it. The
// config home is thrown away before it exists, so Settings cannot reach the
// user's real GameModManager.conf.
struct AppGuard {
  AppGuard() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const std::filesystem::path cfg = "/tmp/gmm_downloads_shortcut/config";
    std::error_code ec;
    std::filesystem::remove_all("/tmp/gmm_downloads_shortcut", ec);
    std::filesystem::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    int argc     = 1;
    char arg0[]  = "test";
    char *argv[] = {arg0, nullptr};
    app          = std::make_unique<QApplication>(argc, argv);
  }
  std::unique_ptr<QApplication> app;
};

void write_file(const std::filesystem::path &path, size_t size) {
  std::ofstream out(path, std::ios::binary);
  out.write("x", 1);
  for (size_t i = 1; i < size; ++i)
    out.put(static_cast<char>('a' + (i % 26)));
}

QTableWidget *table_of(ui::DownloadsTab *tab) {
  return tab->table();
}

int row_named(QTableWidget *table, const char *name) {
  for (int r = 0; r < table->rowCount(); ++r) {
    auto *it = table->item(r, ui::DownloadsTab::Name);
    if (it && it->text() == QLatin1String(name))
      return r;
  }
  return -1;
}
}  // namespace

TEST_CASE("download shortcut maps each state to MO2's keys", "[ui][mo2-parity]") {
  using S       = ui::DownloadState;
  using A       = ui::DownloadShortcut;
  const auto fn = &ui::download_shortcut_for;

  SECTION("a finished row installs on Enter and removes on Delete") {
    for (auto state : {S::Complete, S::Installed}) {
      REQUIRE(fn(state, Qt::Key_Enter) == A::Install);
      REQUIRE(fn(state, Qt::Key_Return) == A::Install);
      REQUIRE(fn(state, Qt::Key_Delete) == A::Remove);
    }
  }

  SECTION("a running row pauses on Space and removes on Delete, never installs") {
    REQUIRE(fn(S::Downloading, Qt::Key_Space) == A::Pause);
    REQUIRE(fn(S::Downloading, Qt::Key_Delete) == A::Remove);
    REQUIRE(fn(S::Downloading, Qt::Key_Enter) == A::None);
  }

  SECTION("a paused or failed row resumes on Space") {
    for (auto state : {S::Paused, S::Failed}) {
      REQUIRE(fn(state, Qt::Key_Space) == A::Resume);
      REQUIRE(fn(state, Qt::Key_Delete) == A::Remove);
      REQUIRE(fn(state, Qt::Key_Enter) == A::None);
    }
  }

  SECTION("unrelated keys do nothing on any row") {
    for (auto state : {S::Downloading, S::Paused, S::Complete, S::Installed, S::Failed,
                       S::Removed}) {
      REQUIRE(fn(state, Qt::Key_A) == A::None);
      REQUIRE(fn(state, Qt::Key_Escape) == A::None);
    }
  }

  SECTION("a removed row answers nothing") {
    REQUIRE(fn(S::Removed, Qt::Key_Enter) == A::None);
    REQUIRE(fn(S::Removed, Qt::Key_Space) == A::None);
    REQUIRE(fn(S::Removed, Qt::Key_Delete) == A::None);
  }
}

TEST_CASE("download keys reach the pause / resume / install signals",
          "[ui][mo2-parity]") {
  AppGuard guard;
  std::error_code ec;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "gmm_dl_shortcut";
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  const auto archive = dir / "Thing.zip";
  write_file(archive, 64);

  ui::DownloadsTab tab;

  // Signals captured by value: the tab is destroyed at the end of the block
  // and a by-reference sink would dangle.
  int pauses = 0, resumes = 0, installs = 0;
  QObject::connect(&tab, &ui::DownloadsTab::pause_requested,
                   [&pauses](const std::string &) {
                     pauses++;
                   });
  QObject::connect(&tab, &ui::DownloadsTab::resume_requested,
                   [&resumes](const std::string &) {
                     resumes++;
                   });
  QObject::connect(&tab, &ui::DownloadsTab::install_requested,
                   [&installs](const std::string &, const std::filesystem::path &,
                               const std::string &, const std::string &, int,
                               const std::string &, const std::string &) {
                     installs++;
                   });

  tab.add_download("id-dl", "Thing.zip", "Manual");
  tab.set_file_path("id-dl", archive);
  auto *table   = table_of(&tab);
  const int row = row_named(table, "Thing.zip");
  REQUIRE(row >= 0);
  table->setCurrentCell(row, ui::DownloadsTab::Name);

  // A Downloading row: Space pauses, Enter does nothing.
  QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
  QApplication::sendEvent(table, &space);
  check(pauses == 1, "Space on a downloading row pauses it");
  check(installs == 0, "Enter is not what fired - nothing installed yet");

  QKeyEvent enter(QEvent::KeyPress, Qt::Key_Enter, Qt::NoModifier);
  QApplication::sendEvent(table, &enter);
  check(installs == 0, "Enter on a downloading row installs nothing");
  check(pauses == 1, "and still does not pause");

  // A Paused row: Space resumes.
  tab.mark_paused("id-dl");
  table->setCurrentCell(row, ui::DownloadsTab::Name);
  QApplication::sendEvent(table, &space);
  check(resumes == 1, "Space on a paused row resumes it");

  // A Complete row: Enter installs (through the same primitive the
  // double-click uses, so one code path owns it).
  tab.mark_complete("id-dl", true);
  table->setCurrentCell(row, ui::DownloadsTab::Name);
  QApplication::sendEvent(table, &enter);
  check(installs == 1, "Enter on a finished row installs it");
  check(pauses == 1, "and does not pause");
  check(resumes == 1, "and does not resume");

  std::filesystem::remove_all(dir, ec);
}

TEST_CASE("download keys do nothing without a selected row", "[ui][mo2-parity]") {
  AppGuard guard;
  std::error_code ec;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "gmm_dl_shortcut_norow";
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);

  ui::DownloadsTab tab;
  int pauses = 0, resumes = 0, installs = 0;
  QObject::connect(&tab, &ui::DownloadsTab::pause_requested,
                   [&pauses](const std::string &) {
                     pauses++;
                   });
  QObject::connect(&tab, &ui::DownloadsTab::resume_requested,
                   [&resumes](const std::string &) {
                     resumes++;
                   });
  QObject::connect(&tab, &ui::DownloadsTab::install_requested,
                   [&installs](const std::string &, const std::filesystem::path &,
                               const std::string &, const std::string &, int,
                               const std::string &, const std::string &) {
                     installs++;
                   });

  auto *table = table_of(&tab);
  table->setCurrentCell(-1, -1);
  QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
  QKeyEvent enter(QEvent::KeyPress, Qt::Key_Enter, Qt::NoModifier);
  QApplication::sendEvent(table, &space);
  QApplication::sendEvent(table, &enter);
  check(pauses == 0 && resumes == 0 && installs == 0,
        "no selection means no action on any of the three keys");

  std::filesystem::remove_all(dir, ec);
}

// Each tooltip case is its own TEST_CASE rather than a SECTION: Catch re-runs
// the whole body per SECTION, which would construct and destroy a second
// QApplication in one process - and QApplication may only exist once per
// process, so the second construction segfaults inside QIconLoader.

// The Name cell's hover text for a queued download: MO2's pendingDownload
// branch (downloadlist.cpp:214-216).
TEST_CASE("a queued download's hover text says it is pending", "[ui][mo2-parity]") {
  AppGuard guard;
  ui::DownloadsTab tab;
  tab.add_download("id-pending", "CoolMod.zip", "Manual");
  const int row = row_named(table_of(&tab), "CoolMod.zip");
  REQUIRE(row >= 0);
  auto *tip = table_of(&tab)->item(row, ui::DownloadsTab::Name);
  REQUIRE(tip != nullptr);
  check(tip->toolTip().contains("Pending download"),
        "MO2's pendingDownload branch names the pending state");
}

// A landed download carries the two things the columns never show: where the
// archive is on disk, and which page it came from.
TEST_CASE("a landed download's hover text names its path and its source page",
          "[ui][mo2-parity]") {
  AppGuard guard;
  std::error_code ec;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "gmm_dl_tooltip_done";
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  const auto archive = dir / "CoolMod.zip";
  write_file(archive, 128);

  ui::DownloadsTab tab;
  tab.add_download("id-done", "CoolMod.zip", "Nexus Mods", archive,
                   "skyrimspecialedition", 1234, "5678");
  const int row = row_named(table_of(&tab), "CoolMod.zip");
  REQUIRE(row >= 0);
  auto *tip = table_of(&tab)->item(row, ui::DownloadsTab::Name);
  REQUIRE(tip != nullptr);
  check(!tip->toolTip().contains("Pending download"),
        "a landed archive is not pending any more");
  check(tip->toolTip().contains("CoolMod.zip"), "the tooltip names the file");
  check(tip->toolTip().contains(archive.string().c_str()),
        "the tooltip gives the on-disk path, which no column shows");
  check(tip->toolTip().contains("nexusmods.com"),
        "the tooltip carries the source page the Open-on action uses");

  std::filesystem::remove_all(dir, ec);
}

// A row with no recorded origin must not grow one.
TEST_CASE("a local download's hover text invents no source page", "[ui][mo2-parity]") {
  AppGuard guard;
  std::error_code ec;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "gmm_dl_tooltip_local";
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  const auto local = dir / "Local.zip";
  write_file(local, 16);

  ui::DownloadsTab tab;
  tab.add_download("id-local", "Local.zip", "Manual", local);
  const int row = row_named(table_of(&tab), "Local.zip");
  REQUIRE(row >= 0);
  const QString tip = table_of(&tab)->item(row, ui::DownloadsTab::Name)->toolTip();
  check(tip.contains(local.string().c_str()), "the local path is shown");
  check(!tip.contains("nexusmods.com"),
        "a local row claims no origin it does not have");

  std::filesystem::remove_all(dir, ec);
}

// A renamed download's tooltip has to follow, or it shows the old name.
// Uses a LANDED row: MO2's pending branch returns the pending notice alone
// (downloadlist.cpp:214-216), so a queued row has no filename in its tooltip
// to go stale - the filename only appears once the archive has landed.
TEST_CASE("a renamed download's hover text follows the new name", "[ui][mo2-parity]") {
  AppGuard guard;
  std::error_code ec;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "gmm_dl_tooltip_rename";
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  const auto archive = dir / "After.zip";
  write_file(archive, 32);

  ui::DownloadsTab tab;
  tab.add_download("id-rename", "Before.zip", "Manual");
  tab.set_file_path("id-rename", archive);
  tab.rename_download("id-rename", "After.zip");
  const int row = row_named(table_of(&tab), "After.zip");
  REQUIRE(row >= 0);
  const QString tip = table_of(&tab)->item(row, ui::DownloadsTab::Name)->toolTip();
  check(tip.contains("After.zip"), "the tooltip shows the new name");
  check(!tip.contains("Before.zip"), "and not the stale one");

  std::filesystem::remove_all(dir, ec);
}