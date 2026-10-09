// Batch actions on the Downloads tab, and hidden rows that survive a
// restart.
//
// MO2's download list has six whole-list actions
// (DownloadListView::downloadContextMenu, downloadlistview.cpp:300-325):
//
//   Delete Installed Downloads...
//   Delete Uninstalled Downloads...
//   Delete All Downloads...
//   Hide Installed...
//   Hide Uninstalled...
//   Hide All...
//   Un-Hide All...          (offered instead of the Hide trio once anything
//                            is hidden)
//
// The Hide family does not delete anything: it flags the rows
// (DownloadInfo::m_Hidden, DownloadManager::isHidden,
// downloadmanager.cpp:1523-1529) so they leave the view, and "Un-Hide All..."
// brings them back. That flag is persisted, so a row the user hid is still
// hidden on the next launch.
//
// Two things this locks down:
//
//   1. The scope rules. An in-flight row has not finished, so it is neither
//      installed nor uninstalled - taking it in either batch would trash an
//      archive that is still being written. "All" does take it, matching
//      MO2's issueDeleteAll.
//
//   2. The consumer. The scope function on its own proves nothing; what
//      matters is that a hidden row actually leaves the table, survives a
//      serialize/deserialize round trip, and comes back on Un-Hide All.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, temp dirs owned
// for the case, no network.
#include "ui/panels/tab_panels.h"

#include <QApplication>
#include <QMenu>
#include <QTableWidget>

#include "ui/settings/settings.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace {

struct TempRoot {
  explicit TempRoot(const char *name)
      : path(std::filesystem::temp_directory_path() / name) {
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path / "config");
    std::filesystem::create_directories(path / "downloads");
  }
  ~TempRoot() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  TempRoot(const TempRoot &)            = delete;
  TempRoot &operator=(const TempRoot &) = delete;

  std::filesystem::path path;
};

void write_zip(const std::filesystem::path &p) {
  std::ofstream out(p, std::ios::binary);
  out << "PK\x03\x04archive";
}

QAction *find_action(QMenu &menu, const char *text) {
  const QString needle = QString::fromLatin1(text);
  for (auto *act : menu.actions())
    if (act->text() == needle)
      return act;
  return nullptr;
}

int row_with_name(QTableWidget *table, const char *name) {
  for (int r = 0; r < table->rowCount(); ++r) {
    auto *it = table->item(r, 0);
    if (it && it->text() == QLatin1String(name))
      return r;
  }
  return -1;
}

// add_context_menu_actions is protected; the existing downloads tests reach it
// the same way.
struct TestDownloadsTab : ui::DownloadsTab {
  using ui::DownloadsTab::add_context_menu_actions;
};

}  // namespace

// MO2 words the six batch actions exactly like this (downloadlistview.cpp:
// 300-325), and each has to exist on the row context menu.
TEST_CASE("downloads batch actions cover MO2's six whole-list actions",
          "[ui][mo2-parity]") {
  using ui::DownloadBatch;
  using ui::DownloadState;

  // Scope: installed and uninstalled are complements over FINISHED rows only.
  // An in-flight row is in neither - trashing an archive that is still being
  // written would destroy the download mid-flight.
  CHECK(ui::download_in_batch(DownloadState::Installed, DownloadBatch::Installed));
  CHECK_FALSE(
      ui::download_in_batch(DownloadState::Installed, DownloadBatch::Uninstalled));
  CHECK(ui::download_in_batch(DownloadState::Complete, DownloadBatch::Uninstalled));
  CHECK(ui::download_in_batch(DownloadState::Failed, DownloadBatch::Uninstalled));
  CHECK_FALSE(
      ui::download_in_batch(DownloadState::Downloading, DownloadBatch::Installed));
  CHECK_FALSE(
      ui::download_in_batch(DownloadState::Downloading, DownloadBatch::Uninstalled));
  CHECK_FALSE(ui::download_in_batch(DownloadState::Paused, DownloadBatch::Installed));
  CHECK_FALSE(ui::download_in_batch(DownloadState::Paused, DownloadBatch::Uninstalled));

  // "All" takes every row that still exists, in flight included (MO2's
  // issueDeleteAll / issueRemoveFromViewAll), but never a Removed entry.
  for (auto s :
       {DownloadState::Downloading, DownloadState::Paused, DownloadState::Complete,
        DownloadState::Installed, DownloadState::Failed})
    CHECK(ui::download_in_batch(s, DownloadBatch::All));
  CHECK_FALSE(ui::download_in_batch(DownloadState::Removed, DownloadBatch::All));

  // Labels.
  CHECK(ui::download_batch_label(DownloadBatch::Installed, true).toStdString() ==
        "Delete Installed Downloads...");
  CHECK(ui::download_batch_label(DownloadBatch::Uninstalled, true).toStdString() ==
        "Delete Uninstalled Downloads...");
  CHECK(ui::download_batch_label(DownloadBatch::All, true).toStdString() ==
        "Delete All Downloads...");
  CHECK(ui::download_batch_label(DownloadBatch::Installed, false).toStdString() ==
        "Hide Installed...");
  CHECK(ui::download_batch_label(DownloadBatch::Uninstalled, false).toStdString() ==
        "Hide Uninstalled...");
  CHECK(ui::download_batch_label(DownloadBatch::All, false).toStdString() ==
        "Hide All...");
}

// The context menu must actually offer these. This is the wiring half: a
// scope function nothing calls would pass the test above and do nothing.
TEST_CASE("the downloads context menu offers the batch actions", "[ui][mo2-parity]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  TempRoot root("gmm_dl_batch_menu");
  qputenv("XDG_CONFIG_HOME", (root.path / "config").c_str());
  int argc     = 1;
  char argv0[] = "test";
  char *argv[] = {argv0, nullptr};
  QApplication app(argc, argv);

  const auto zip = root.path / "downloads" / "A.zip";
  write_zip(zip);

  TestDownloadsTab tab;
  tab.add_download("a", "A", "Manual", zip);
  tab.mark_complete("a", true);

  QMenu menu;
  tab.add_context_menu_actions(menu, "a");
  CHECK(find_action(menu, "Delete Installed Downloads...") != nullptr);
  CHECK(find_action(menu, "Delete Uninstalled Downloads...") != nullptr);
  CHECK(find_action(menu, "Delete All Downloads...") != nullptr);
  CHECK(find_action(menu, "Hide Installed...") != nullptr);
  CHECK(find_action(menu, "Hide Uninstalled...") != nullptr);
  CHECK(find_action(menu, "Hide All...") != nullptr);
  // Nothing is hidden yet, so there is nothing to un-hide.
  CHECK(find_action(menu, "Un-Hide All...") == nullptr);
}

// The consumer: a hidden row leaves the table, is NOT deleted, survives the
// manifest round trip, and comes back on Un-Hide All.
TEST_CASE("a hidden download stays hidden across a restart and can be un-hidden",
          "[ui][mo2-parity]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  TempRoot root("gmm_dl_hidden");
  qputenv("XDG_CONFIG_HOME", (root.path / "config").c_str());
  int argc     = 1;
  char argv0[] = "test";
  char *argv[] = {argv0, nullptr};
  QApplication app(argc, argv);

  const auto dl  = root.path / "downloads";
  const auto zip = dl / "Keep.zip";
  write_zip(zip);

  std::string manifest;
  {
    TestDownloadsTab tab;
    tab.set_downloads_dir(dl);
    tab.add_download("keep", "Keep", "Manual", zip);
    tab.mark_complete("keep", true);

    // Hide Installed... covers nothing here (nothing is installed); Hide
    // Uninstalled... takes the one finished row. MO2's Hide family does not
    // delete, so the archive must still be on disk afterwards.
    QMenu menu;
    tab.add_context_menu_actions(menu, "keep");
    auto *act = find_action(menu, "Hide Uninstalled...");
    REQUIRE(act != nullptr);
    act->trigger();

    const int row = row_with_name(tab.table(), "Keep");
    REQUIRE(row >= 0);
    CHECK(tab.table()->isRowHidden(row));
    CHECK(std::filesystem::exists(zip));

    // Now that something is hidden, the Hide trio is replaced by Un-Hide All.
    QMenu after;
    tab.add_context_menu_actions(after, "keep");
    CHECK(find_action(after, "Un-Hide All...") != nullptr);
    CHECK(find_action(after, "Hide All...") == nullptr);

    manifest = tab.serialize();
  }

  // Restart: the hidden flag must come back, and the row must still be hidden
  // rather than reappearing. Manifest first, then the dir scan - the order
  // DownloadsController uses (downloads_controller.cpp:390-403), so the scan
  // does not add the archive as a fresh untracked row and shadow the entry
  // the manifest restored.
  {
    TestDownloadsTab tab;
    tab.deserialize(manifest, dl);
    tab.set_downloads_dir(dl);
    const int row = row_with_name(tab.table(), "Keep");
    REQUIRE(row >= 0);
    CHECK(tab.table()->isRowHidden(row));

    QMenu menu;
    tab.add_context_menu_actions(menu, "keep");
    auto *unhide = find_action(menu, "Un-Hide All...");
    REQUIRE(unhide != nullptr);
    unhide->trigger();
    CHECK(!tab.table()->isRowHidden(row));

    // And the flag is cleared in the manifest, so the next launch agrees.
    const std::string after = tab.serialize();
    TestDownloadsTab third;
    third.deserialize(after, dl);
    third.set_downloads_dir(dl);
    const int row2 = row_with_name(third.table(), "Keep");
    REQUIRE(row2 >= 0);
    CHECK(!third.table()->isRowHidden(row2));
  }
}
