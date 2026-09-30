// The Downloads tab must not describe a transfer that is not happening, and
// must not fetch the same file twice.
//
// Three failure modes, all reachable without anything unusual:
//
//   1. A manifest written while a transfer was in flight (or paused) is loaded
//      on the next launch. The transfer died with the process, and the link
//      maps resume needs are in-memory only, so the row cannot be continued.
//      Restored as Downloading/Paused it also wedges has_active_download(),
//      which stops scan_downloads_dir() - so the folder watcher stops
//      auto-detecting archives for the rest of the session.
//
//   2. A download link arrives twice (re-clicked browser button, second
//      window, duplicated handler). Re-queuing a FINISHED download resumes
//      from a complete file, gets HTTP 416, and Network::Manager::download
//      removes the destination - the user's archive is gone. Re-queuing an
//      IN-FLIGHT one opens the same file for writing twice.
//
//   3. A Nexus collection link parses as valid (the router only requires a
//      domain) with mod_id and file_id both 0, and queues a "Mod #0 - file 0"
//      row that can only fail.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, scope-guarded temp
// dir, no network.
#include "ui/panels/tab_panels.h"

#include <QApplication>
#include <QTableWidget>
#include <QTableWidgetItem>

#include "ui/settings/settings.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Owns a throwaway root for the case and removes it on the way out, so a run
// leaves nothing behind under /tmp.
struct TempRoot {
  explicit TempRoot(const char *name) : path(std::filesystem::temp_directory_path() / name) {
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

// A row's Status cell is the only public observable for its state, so read the
// label the tab renders rather than reaching into DownloadEntry.
static std::string status_text(ui::DownloadsTab &tab, int row) {
  auto *item = tab.table()->item(row, ui::DownloadsTab::Status);
  return item ? item->text().toStdString() : std::string();
}

}  // namespace

// A manifest entry that claims a transfer is running describes a process that
// no longer exists. It must come back Failed, and it must not wedge the
// downloads dir scan.
TEST_CASE("a manifest row restored as in-flight comes back failed and does not wedge the scan",
          "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  TempRoot root("gmm_dl_stale_state");
  qputenv("XDG_CONFIG_HOME", (root.path / "config").c_str());
  int argc     = 1;
  char argv0[] = "test";
  char *argv[] = {argv0, nullptr};
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const auto zip = root.path / "downloads" / "Interrupted Mod.zip";
  std::ofstream out(zip, std::ios::binary);
  out << "partial";
  out.close();

  ui::DownloadsTab tab;
  // State 1 = Downloading, 3 = Paused, per the DownloadState enum order.
  const std::string manifest = std::string("[{\"id\":\"stale-1\",\"name\":\"Stale\","
                                           "\"source\":\"Nexus Mods\",\"size\":\"\","
                                           "\"file_path\":\"") +
                               zip.string() +
                               "\",\"state\":1,\"total_size\":7,"
                               "\"parent_mod_id\":\"10\",\"file_id\":20,\"domain\":"
                               "\"skyrimspecialedition\",\"category\":\"\",\"page_url\":"
                               "\"\"}]";
  tab.deserialize(manifest, root.path / "downloads");

  REQUIRE(tab.table()->rowCount() == 1);
  CHECK(status_text(tab, 0) == "Failed");
  // The wedge: has_active_download() counted both stale states, so the scan
  // guard refused to run and the folder watcher went dead for the session.
  CHECK_FALSE(tab.has_active_download());

  // The scan runs, so an archive nobody tracked still surfaces. (The stale
  // row's own archive must NOT gain a second row - it already backs a
  // tracked entry.)
  const auto fresh = root.path / "downloads" / "Fresh Drop.zip";
  std::ofstream fresh_out(fresh, std::ios::binary);
  fresh_out << "fresh";
  fresh_out.close();
  tab.set_downloads_dir(root.path / "downloads");
  CHECK(tab.table()->rowCount() == 2);
  // Guarded so a regression reports the rowCount CHECK above instead of
  // segfaulting on the missing row.
  auto *fresh_item = tab.table()->item(1, ui::DownloadsTab::Name);
  CHECK(fresh_item != nullptr);
  if (fresh_item)
    CHECK(fresh_item->text() == "Fresh Drop");
}

// A repeat of a link for a download that already finished must not be
// re-fetched: the resume would ask for bytes past the end of a complete file
// and the failed request takes the archive with it.
TEST_CASE("a repeat link does not re-fetch a download that is already on disk", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  TempRoot root("gmm_dl_refetch_guard");
  qputenv("XDG_CONFIG_HOME", (root.path / "config").c_str());
  int argc     = 1;
  char argv0[] = "test";
  char *argv[] = {argv0, nullptr};
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const auto zip = root.path / "downloads" / "Done Mod.zip";
  std::ofstream out(zip, std::ios::binary);
  out << "complete archive";
  out.close();

  ui::DownloadsTab tab;
  // add_download reports whether it made the row, so a caller can tell a new
  // download from a repeat of one it already queued.
  CHECK(tab.add_download("10-20", "Done Mod", "Nexus Mods", zip, "skyrimspecialedition",
                         20, "10"));
  CHECK_FALSE(tab.add_download("10-20", "Done Mod", "Nexus Mods", zip,
                               "skyrimspecialedition", 20, "10"));
  CHECK(tab.table()->rowCount() == 1);

  // Downloading: a second transfer would open the same file for writing while
  // the first appends to it.
  CHECK(tab.blocks_refetch("10-20"));
  tab.mark_complete("10-20", true);
  // Complete: re-fetching resumes from a complete file.
  CHECK(tab.blocks_refetch("10-20"));
  // Installed: same archive, same answer.
  tab.mark_installed("10-20");
  CHECK(tab.blocks_refetch("10-20"));

  // Failed and Paused are the two states a new link may still act on: there is
  // no Retry action on a row, so re-queuing is how both are recovered.
  tab.mark_paused("10-20");
  CHECK_FALSE(tab.blocks_refetch("10-20"));
  tab.mark_complete("10-20", false);
  CHECK_FALSE(tab.blocks_refetch("10-20"));

  // An id the tab never saw is not blocked.
  CHECK_FALSE(tab.blocks_refetch("never-seen"));
}
