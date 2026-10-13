// The per-file download meta, and the one-way migration off the single
// .download_manifest.json.
//
// MO2 keeps download state in a "<archive>.meta" file beside each archive,
// QSettings IniFormat (DownloadManager::createMetaFile / DownloadInfo::
// createFromMeta, downloadmanager.cpp:88-164 and 1771-1799), and rebuilds the
// list by scanning the dir for "*.meta" (downloadmanager.cpp:412). This test
// pins the parts of that format we depend on, the restore round-trip that
// actually moves a user's list across, and the four ways it can go wrong.
//
//   - round trip: a tab's rows become metas, the metas are written, the legacy
//     manifest is gone, and a FRESH tab restores from the metas alone with
//     every field unchanged - the same code path a migrated install takes.
//   - a corrupt meta (no readable keys) restores as Failed, never as the
//     Complete that a default-constructed meta would otherwise read as, and
//     the row is not dropped.
//   - a meta whose archive is gone still restores its row, as Failed.
//     MO2 deletes these orphans outright (downloadmanager.cpp:409-422); we
//     do not, and this test is what holds that difference in place.
//   - an archive with no meta still surfaces through the dir scan, so the
//     migration never turns a manually downloaded archive invisible.
//   - a migration that cannot write every meta keeps the legacy manifest, so
//     the next run repeats it with nothing lost.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, no network, no
// access to the real instance downloads dirs.
#include "ui/panels/downloads_tab.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTableWidget>
#include <QTableWidgetItem>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

#include "ui/settings/settings.h"

namespace {

constexpr const char *kRoot = "/tmp/gmm_download_meta";

// Every case starts from the same clean slate: a throwaway config dir (the
// QSettings sandbox - never the real user config) and a throwaway downloads
// dir (never the real instance data).
struct Sandbox {
  std::filesystem::path dl;
  Sandbox() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const std::filesystem::path cfg = std::filesystem::path(kRoot) / "config";
    std::filesystem::remove_all(kRoot);
    std::filesystem::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.string().c_str());
    dl = std::filesystem::path(kRoot) / "dl";
    std::filesystem::create_directories(dl);
  }
  void app(int &argc, char **argv) {
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");
    (void)argc;
    (void)argv;
  }
};

void write_file(const std::filesystem::path &path, size_t size) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  if (!out)
    return;
  out.write("x", 1);
  for (size_t i = 1; i < size; ++i)
    out.put(static_cast<char>('a' + (i % 26)));
}

void write_text(const std::filesystem::path &path, const std::string &text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << text;
}

// Names the first field two metas disagree on, empty when they agree. Reports
// the FIELD rather than a bare false, so a failure says which part of the
// format lost the value instead of just "not equal".
std::string meta_diff(const ui::DownloadMeta &a, const ui::DownloadMeta &b) {
  const std::vector<std::tuple<std::string, std::string, std::string>> fields = {
      {"archive", a.archive.string(), b.archive.string()},
      {"id", a.id, b.id},
      {"name", a.name, b.name},
      {"source", a.source, b.source},
      {"page_url", a.page_url, b.page_url},
      {"parent_mod_id", a.parent_mod_id, b.parent_mod_id},
      {"file_id", std::to_string(a.file_id), std::to_string(b.file_id)},
      {"nexus_domain", a.nexus_domain, b.nexus_domain},
      {"category", a.category, b.category},
      {"total_size", std::to_string(a.total_size), std::to_string(b.total_size)},
      {"partial_file", a.partial_file, b.partial_file},
      {"state", std::to_string(static_cast<int>(a.state)),
       std::to_string(static_cast<int>(b.state))},
      {"hidden", a.hidden ? "1" : "0", b.hidden ? "1" : "0"},
  };
  for (const auto &[name, left, right] : fields) {
    if (left != right)
      return name;
  }
  return {};
}

const ui::DownloadMeta *find_meta(const std::vector<ui::DownloadMeta> &metas,
                                  const std::string &id) {
  for (const auto &m : metas)
    if (m.id == id)
      return &m;
  return nullptr;
}

int row_of(ui::DownloadsTab *tab, const std::string &name) {
  for (int r = 0; r < tab->table()->rowCount(); ++r) {
    auto *it = tab->table()->item(r, ui::DownloadsTab::Name);
    if (it && it->text().toStdString() == name)
      return r;
  }
  return -1;
}

}  // namespace

TEST_CASE("download meta round-trips through the real restore path", "[ui]") {
  Sandbox sandbox;
  int test_argc     = 1;
  char test_argv0[] = "download_meta_test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  sandbox.app(test_argc, test_argv);
  const std::filesystem::path dl = sandbox.dl;

  // Three archives on disk, three rows covering the states the meta can carry:
  // a finished Nexus row, an installed Nexus row, and a manual archive.
  const auto nexus_zip  = dl / "Some Mod-32444-11-1234.zip";
  const auto inst_7z    = dl / "Installed Mod-555-2-9.7z";
  const auto manual_zip = dl / "Dragged In.zip";
  write_file(nexus_zip, 4096);
  write_file(inst_7z, 8192);
  write_file(manual_zip, 2048);

  {
    ui::DownloadsTab tab;
    tab.add_download("32444-1234", "Some Mod", "Nexus Mods", nexus_zip,
                     "skyrimspecialedition", 1234, "32444");
    tab.mark_complete("32444-1234", true);

    tab.add_download("555-9", "Installed Mod", "Nexus Mods", inst_7z,
                     "skyrimspecialedition", 9, "555");
    tab.mark_complete("555-9", true);
    tab.mark_installed("555-9");

    tab.add_download("Dragged In", "Dragged In", "Manual", manual_zip, "", 0, "");

    tab.set_downloads_dir(dl);

    const auto before = tab.to_metas();
    REQUIRE(before.size() == 3);

    // The legacy manifest is gone by the time the metas are read back: this is
    // the state a migrated install is in from the second run onwards.
    const auto legacy = dl / ".download_manifest.json";
    write_text(legacy, "[]");
    CHECK(ui::migrate_download_state(before, dl, legacy));
    CHECK_FALSE(std::filesystem::exists(legacy));

    const auto after = ui::read_download_metas(dl);
    REQUIRE(after.size() == 3);

    // A FRESH tab restores from the metas alone, through the same deserialize()
    // the legacy path uses.
    ui::DownloadsTab restored;
    restored.deserialize(ui::metas_to_manifest_json(after, dl), dl);
    const auto round_tripped = restored.to_metas();
    REQUIRE(round_tripped.size() == 3);

    // The only field that legitimately moves is total_size: these rows were
    // never driven by a live worker, so the tab had no recorded size, and
    // deserialize() recovers it from the bytes on disk (downloads_tab.cpp's
    // resolved_size fallback). A row still in flight when it was saved also
    // moves, and only downwards: it comes back Failed, because nothing is
    // fetching it any more. Every other field must survive byte for byte.
    for (const auto &original : before) {
      const ui::DownloadMeta *got = find_meta(round_tripped, original.id);
      REQUIRE(got != nullptr);
      auto expected       = original;
      expected.total_size = got->total_size;
      if (original.state == ui::DownloadState::Downloading ||
          original.state == ui::DownloadState::Paused)
        expected.state = ui::DownloadState::Failed;
      const auto diff = meta_diff(expected, *got);
      INFO("meta id: " << original.id);
      INFO("first differing field: " << (diff.empty() ? "<none>" : diff));
      CHECK(diff.empty());
    }

    // An in-flight row must NEVER come back Complete. That is the exact
    // "stale state marks a short file finished" failure the per-file meta was
    // adopted to remove, and "Dragged In" was left Downloading above.
    const ui::DownloadMeta *inflight = find_meta(round_tripped, "Dragged In");
    REQUIRE(inflight != nullptr);
    CHECK(inflight->state == ui::DownloadState::Failed);

    // The invariant that actually matters: once migrated, the state does not
    // drift. Write the restored rows back out, read them in again, and every
    // field must come back unchanged - this is what a user's list does on
    // every run after the first.
    REQUIRE(ui::write_download_metas(round_tripped, dl));
    ui::DownloadsTab again;
    again.deserialize(ui::metas_to_manifest_json(ui::read_download_metas(dl), dl), dl);
    const auto twice = again.to_metas();
    REQUIRE(twice.size() == 3);
    for (const auto &first : round_tripped) {
      const ui::DownloadMeta *got = find_meta(twice, first.id);
      REQUIRE(got != nullptr);
      const auto diff = meta_diff(first, *got);
      INFO("meta id: " << first.id);
      INFO("first differing field: " << (diff.empty() ? "<none>" : diff));
      CHECK(diff.empty());
    }

    // And the states came back as themselves, not all collapsed to one.
    const auto *inst = find_meta(twice, "555-9");
    REQUIRE(inst != nullptr);
    CHECK(inst->state == ui::DownloadState::Installed);
    const auto *done = find_meta(twice, "32444-1234");
    REQUIRE(done != nullptr);
    CHECK(done->state == ui::DownloadState::Complete);
  }

  // The meta sits beside its archive and is named after it, MO2 style.
  CHECK(std::filesystem::exists(dl / "Some Mod-32444-11-1234.zip.meta"));

  // MO2 reads its state out of these keys. Pin the exact key names so a
  // rename cannot quietly break interop with MO2's reader.
  QSettings meta(
      QString::fromStdString((dl / "Some Mod-32444-11-1234.zip.meta").string()),
      QSettings::IniFormat);
  CHECK(meta.value("name").toString().toStdString() == "Some Mod");
  CHECK(meta.value("modID").toString().toStdString() == "32444");
  CHECK(meta.value("fileID").toInt() == 1234);
  CHECK(meta.value("gameName").toString().toStdString() == "skyrimspecialedition");
  CHECK(meta.value("repository").toString().toStdString() == "Nexus Mods");
  // MO2's own read path (downloadmanager.cpp:118-126) derives READY from
  // "nothing set", which is what a finished download must write.
  CHECK_FALSE(meta.value("installed", false).toBool());
  CHECK_FALSE(meta.value("paused", false).toBool());
  CHECK_FALSE(meta.value("removed", false).toBool());
  // totalSize is ours, and the P0 archive_is_complete() fix depends on it.
  CHECK(meta.value("totalSize").toLongLong() == 4096);
}

TEST_CASE("a corrupt meta restores as failed, never as complete", "[ui]") {
  Sandbox sandbox;
  int test_argc     = 1;
  char test_argv0[] = "download_meta_test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  sandbox.app(test_argc, test_argv);

  const std::filesystem::path dl = sandbox.dl;
  const auto archive             = dl / "Damaged Mod-111-1-1.zip";
  write_file(archive, 4096);

  // A meta truncated to garbage: QSettings parses no keys at all.
  write_text(archive.string() + ".meta", "\x01\x02 not an ini \x7f\n[[[\n");

  const auto metas = ui::read_download_metas(dl);
  REQUIRE(metas.size() == 1);
  // The row is KEPT - dropping it would be how a corrupt file loses a
  // download - and it comes back Failed, because Failed is the one state
  // that admits the bytes are not known good. Complete here would be the P0
  // failure class this format was adopted to remove.
  CHECK(metas[0].state == ui::DownloadState::Failed);
  CHECK(metas[0].archive == archive);

  // An empty meta is the same case: no keys, no state to read.
  write_text(archive.string() + ".meta", "");
  const auto empty = ui::read_download_metas(dl);
  REQUIRE(empty.size() == 1);
  CHECK(empty[0].state == ui::DownloadState::Failed);

  // A truncated write that kept SOME keys is not a corrupt file - it is a
  // partial one, and the flags present must still be honoured rather than
  // being discarded wholesale.
  write_text(archive.string() + ".meta", "[General]\ninstalled=true\n");
  const auto partial = ui::read_download_metas(dl);
  REQUIRE(partial.size() == 1);
  CHECK(partial[0].state == ui::DownloadState::Installed);
}

TEST_CASE("a meta with no archive keeps its row", "[ui]") {
  Sandbox sandbox;
  int test_argc     = 1;
  char test_argv0[] = "download_meta_test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  sandbox.app(test_argc, test_argv);

  const std::filesystem::path dl = sandbox.dl;
  // The archive is NOT created: the user deleted it out from under us, or the
  // download never finished. The meta is all that is left of the row.
  const auto gone = dl / "Deleted Mod-777-3-3.zip";

  ui::DownloadMeta meta;
  meta.archive       = gone;
  meta.id            = "777-3";
  meta.name          = "Deleted Mod";
  meta.source        = "Nexus Mods";
  meta.parent_mod_id = "777";
  meta.file_id       = 3;
  meta.nexus_domain  = "skyrimspecialedition";
  meta.total_size    = 12345;
  meta.state         = ui::DownloadState::Complete;
  REQUIRE(ui::write_download_meta(meta, ui::download_meta_path(meta, dl)));

  const auto metas = ui::read_download_metas(dl);
  REQUIRE(metas.size() == 1);
  // MO2 would delete this orphan outright (downloadmanager.cpp:409-422). We
  // keep it: the row is still a remembered download, and the identity fields
  // are what let the user recognise and remove it.
  CHECK(metas[0].id == "777-3");
  CHECK(metas[0].parent_mod_id == "777");
  CHECK(metas[0].total_size == 12345);

  // Through the restore path it comes back Failed, not Complete: a row whose
  // bytes are gone must not claim to be a finished download.
  ui::DownloadsTab tab;
  tab.deserialize(ui::metas_to_manifest_json(metas, dl), dl);
  const auto restored = tab.to_metas();
  REQUIRE(restored.size() == 1);
  CHECK(restored[0].state == ui::DownloadState::Failed);

  // But a full wiring does NOT keep it, and that is pre-existing behaviour we
  // are not changing here: scan_downloads_dir() drops any row whose archive
  // is gone from the dir (downloads_tab.cpp:1052) and removes its meta with
  // it. This was equally true of the single manifest - the row was restored
  // by deserialize() and then dropped by the same scan - so the migration
  // neither introduces nor fixes it. Pinned here so the difference between
  // "the reader keeps it" and "the tab keeps it" is written down rather than
  // assumed.
  tab.set_downloads_dir(dl);
  CHECK(tab.to_metas().empty());
}

TEST_CASE("an archive with no meta still surfaces", "[ui]") {
  Sandbox sandbox;
  int test_argc     = 1;
  char test_argv0[] = "download_meta_test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  sandbox.app(test_argc, test_argv);

  const std::filesystem::path dl = sandbox.dl;
  // A manually downloaded archive: bytes on disk, no meta anywhere.
  write_file(dl / "No Meta Here.zip", 2048);

  ui::DownloadsTab tab;
  tab.set_downloads_dir(dl);
  REQUIRE(tab.table()->rowCount() == 1);
  CHECK(row_of(&tab, "No Meta Here") >= 0);

  // And once a meta exists it is not duplicated by the scan.
  ui::DownloadMeta meta;
  meta.archive = dl / "No Meta Here.zip";
  meta.id      = "No Meta Here";
  meta.name    = "No Meta Here";
  meta.source  = "Manual";
  meta.state   = ui::DownloadState::Complete;
  REQUIRE(ui::write_download_meta(meta, ui::download_meta_path(meta, dl)));

  ui::DownloadsTab second;
  second.deserialize(ui::metas_to_manifest_json(ui::read_download_metas(dl), dl), dl);
  second.set_downloads_dir(dl);
  CHECK(second.table()->rowCount() == 1);
}

TEST_CASE("a half-written migration keeps the legacy manifest", "[ui]") {
  Sandbox sandbox;
  int test_argc     = 1;
  char test_argv0[] = "download_meta_test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  sandbox.app(test_argc, test_argv);

  const std::filesystem::path dl = sandbox.dl;
  const auto good                = dl / "Fine Mod-1-1-1.zip";
  const auto stuck               = dl / "Stuck Mod-2-2-2.zip";
  write_file(good, 1024);
  write_file(stuck, 1024);

  // Force one write to fail by putting a DIRECTORY where its meta must go, so
  // QSettings cannot create the file. Everything else writes normally.
  std::filesystem::create_directories(stuck.string() + ".meta");

  const std::string legacy_body = R"([{"id":"1-1","name":"Fine Mod","state":3}])";
  const auto legacy             = dl / ".download_manifest.json";
  write_text(legacy, legacy_body);

  ui::DownloadMeta fine;
  fine.archive = good;
  fine.id      = "1-1";
  fine.name    = "Fine Mod";
  fine.source  = "Nexus Mods";
  fine.state   = ui::DownloadState::Installed;

  ui::DownloadMeta stuck_meta;
  stuck_meta.archive = stuck;
  stuck_meta.id      = "2-2";
  stuck_meta.name    = "Stuck Mod";
  stuck_meta.source  = "Nexus Mods";
  stuck_meta.state   = ui::DownloadState::Installed;

  const std::vector<ui::DownloadMeta> metas = {fine, stuck_meta};

  // The migration reports failure...
  CHECK_FALSE(ui::migrate_download_state(metas, dl, legacy));
  // ...the legacy manifest is untouched, so the next run repeats the whole
  // thing rather than finding neither the old state nor a new one...
  REQUIRE(std::filesystem::exists(legacy));
  {
    std::ifstream in(legacy);
    std::string body((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    CHECK(body == legacy_body);
  }
  // ...and the entry that COULD be written was, so a retry is not starting
  // from nothing.
  CHECK(std::filesystem::exists(dl / "Fine Mod-1-1-1.zip.meta"));

  // Clear the obstruction and retry: now it completes and the legacy file goes.
  std::filesystem::remove_all(stuck.string() + ".meta");
  CHECK(ui::migrate_download_state(metas, dl, legacy));
  CHECK_FALSE(std::filesystem::exists(legacy));
  CHECK(std::filesystem::exists(dl / "Stuck Mod-2-2-2.zip.meta"));
}

TEST_CASE("removing a row removes its meta", "[ui]") {
  Sandbox sandbox;
  int test_argc     = 1;
  char test_argv0[] = "download_meta_test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  sandbox.app(test_argc, test_argv);

  const std::filesystem::path dl = sandbox.dl;
  const auto archive             = dl / "Removable-1-1-1.zip";
  write_file(archive, 1024);

  ui::DownloadsTab tab;
  tab.add_download("1-1", "Removable", "Nexus Mods", archive, "skyrimspecialedition", 1,
                   "1");
  tab.mark_complete("1-1", true);
  tab.set_downloads_dir(dl);

  REQUIRE(ui::write_download_metas(tab.to_metas(), dl));
  const auto meta_path = dl / "Removable-1-1-1.zip.meta";
  REQUIRE(std::filesystem::exists(meta_path));

  tab.remove_entry("1-1");
  // The meta outlives the archive by design, so an explicit Remove has to
  // take it too - otherwise the row comes straight back on the next launch.
  CHECK_FALSE(std::filesystem::exists(meta_path));
  CHECK_FALSE(std::filesystem::exists(archive));
}