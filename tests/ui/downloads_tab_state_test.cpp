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
  // State 0 = Downloading (in flight when the process died), per the
  // DownloadState enum order. A Paused row with a rebuildable identity is a
  // deliberate pause and comes back resumable instead - that is covered by
  // download_meta_test's pause-survival case.
  const std::string manifest = std::string("[{\"id\":\"stale-1\",\"name\":\"Stale\","
                                           "\"source\":\"Nexus Mods\",\"size\":\"\","
                                           "\"file_path\":\"") +
                               zip.string() +
                               "\",\"state\":0,\"total_size\":7,"
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
// and the failed request takes the archive with it. But a row this link just
// created is not a finished download - it is the pre-fetch state, and it must
// not refuse the transfer it was created for.
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
  // download from a repeat of one it already queued. A row it made is NOT
  // subject to the guard - that is the whole point of the return value.
  REQUIRE(tab.add_download("10-20", "Done Mod", "Nexus Mods", zip,
                           "skyrimspecialedition", 20, "10"));
  // blocks_refetch alone would say yes here (the row is Downloading), which is
  // why callers gate on the add_download result.
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

// A link that arrives for a mod the tab has no row for must be allowed to
// fetch. This is the reported P0: add_download() created the row in
// Downloading, blocks_refetch() refused it, and every download stopped at
// "already queued or on disk" having moved zero bytes.
TEST_CASE("a fresh link is not refused by the tab's own new row", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  TempRoot root("gmm_dl_fresh_link");
  qputenv("XDG_CONFIG_HOME", (root.path / "config").c_str());
  int argc     = 1;
  char argv0[] = "test";
  char *argv[] = {argv0, nullptr};
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  ui::DownloadsTab tab;
  const bool added =
      tab.add_download("30379-795992", "Mod #30379 - file 795992", "Nexus Mods", {},
                       "skyrimspecialedition", 795992, "30379");
  REQUIRE(added);
  CHECK(tab.table()->rowCount() == 1);

  // The guard a caller applies when the row already existed. A row this call
  // created is exempt, so the transfer runs.
  const bool already_present = !added;
  CHECK_FALSE(already_present);
  const bool should_refetch = !already_present || !tab.blocks_refetch("30379-795992");
  CHECK(should_refetch);

  // A second click on the same browser button lands on the existing row and IS
  // blocked: that is the case the guard is for.
  const bool added_again =
      tab.add_download("30379-795992", "Mod #30379 - file 795992", "Nexus Mods", {},
                       "skyrimspecialedition", 795992, "30379");
  CHECK_FALSE(added_again);
  CHECK(tab.blocks_refetch("30379-795992"));
}

// "On disk" has to mean bytes. A file that exists but is empty, or is shorter
// than the transfer that wrote it, is not a finished download - restoring it as
// Complete makes blocks_refetch refuse every future link against bytes that
// were never fetched.
TEST_CASE("an empty or short archive does not restore as a finished download", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  TempRoot root("gmm_dl_partial_archive");
  qputenv("XDG_CONFIG_HOME", (root.path / "config").c_str());
  int argc     = 1;
  char argv0[] = "test";
  char *argv[] = {argv0, nullptr};
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const auto dir = root.path / "downloads";
  // Zero bytes: exists, is a regular file, is not a download.
  const auto empty = dir / "Empty Mod.zip";
  std::ofstream(empty, std::ios::binary).close();
  // Half of the 100 bytes the manifest says the transfer reported.
  const auto shortf = dir / "Short Mod.zip";
  {
    std::ofstream o(shortf, std::ios::binary);
    o << std::string(50, 'x');
  }
  // All of it.
  const auto whole = dir / "Whole Mod.zip";
  {
    std::ofstream o(whole, std::ios::binary);
    o << std::string(100, 'x');
  }

  ui::DownloadsTab tab;
  // State 2 = Complete, per the DownloadState enum order.
  const std::string manifest =
      std::string("[{\"id\":\"empty-1\",\"name\":\"Empty\",\"source\":\"Nexus Mods\","
                  "\"size\":\"\",\"file_path\":\"") +
      empty.string() +
      "\",\"state\":2,\"total_size\":100,\"parent_mod_id\":\"1\",\"file_id\":2,"
      "\"domain\":\"skyrimspecialedition\",\"category\":\"\",\"page_url\":\"\"},"
      "{\"id\":\"short-1\",\"name\":\"Short\",\"source\":\"Nexus Mods\",\"size\":\"\","
      "\"file_path\":\"" +
      shortf.string() +
      "\",\"state\":2,\"total_size\":100,\"parent_mod_id\":\"1\",\"file_id\":3,"
      "\"domain\":\"skyrimspecialedition\",\"category\":\"\",\"page_url\":\"\"},"
      "{\"id\":\"whole-1\",\"name\":\"Whole\",\"source\":\"Nexus Mods\",\"size\":\"\","
      "\"file_path\":\"" +
      whole.string() +
      "\",\"state\":2,\"total_size\":100,\"parent_mod_id\":\"1\",\"file_id\":4,"
      "\"domain\":\"skyrimspecialedition\",\"category\":\"\",\"page_url\":\"\"}]";
  tab.deserialize(manifest, dir);

  REQUIRE(tab.table()->rowCount() == 3);
  CHECK(status_text(tab, 0) == "Failed");
  CHECK(status_text(tab, 1) == "Failed");
  // A finished row reads "Install" (the action it offers), not "Complete".
  CHECK(status_text(tab, 2) == "Install");

  // And the practical consequence: a link for the empty one is not refused.
  CHECK_FALSE(tab.blocks_refetch("empty-1"));
  CHECK_FALSE(tab.blocks_refetch("short-1"));
  CHECK(tab.blocks_refetch("whole-1"));
}

// A 0-byte file dropped in the downloads dir is not a mod. Surfacing it as a
// finished row both shows the user a downloadable archive that is not there and
// makes a repeat link refuse against nothing.
TEST_CASE("an empty archive dropped in the downloads dir is not surfaced", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  TempRoot root("gmm_dl_empty_drop");
  qputenv("XDG_CONFIG_HOME", (root.path / "config").c_str());
  int argc     = 1;
  char argv0[] = "test";
  char *argv[] = {argv0, nullptr};
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const auto dir   = root.path / "downloads";
  const auto empty = dir / "Empty.zip";
  std::ofstream(empty, std::ios::binary).close();
  const auto real = dir / "Real.zip";
  {
    std::ofstream o(real, std::ios::binary);
    o << "bytes";
  }

  ui::DownloadsTab tab;
  tab.set_downloads_dir(dir);
  // The empty one is left out, the real one is surfaced as Complete.
  CHECK(tab.table()->rowCount() == 1);
  auto *name = tab.table()->item(0, ui::DownloadsTab::Name);
  REQUIRE(name != nullptr);
  CHECK(name->text() == "Real");
  CHECK(status_text(tab, 0) == "Install");
}
