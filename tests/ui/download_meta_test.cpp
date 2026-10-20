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
//   - MO2's orphan sweep (downloadmanager.cpp:409-422) deletes a meta with no
//     archive on disk, and so do we - but NOT when the meta says the download
//     is still in progress (paused, or beside its partial), which MO2 sweeps
//     by accident. That swept/kept split is the whole point of the case.
//   - an archive with no meta still surfaces through the dir scan, so the
//     migration never turns a manually downloaded archive invisible.
//   - a migration that cannot write every meta keeps the legacy manifest, so
//     the next run repeats it with nothing lost.
//
// Hermetic: offscreen platform, throwaway HOME + XDG_CONFIG_HOME +
// XDG_DATA_HOME, no network, no access to the real instance downloads dirs.
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

constexpr const char *kRoot = "/tmp/opencode/gmm_download_meta";

// Every case starts from the same clean slate: a throwaway config dir (the
// QSettings sandbox - never the real user config) and a throwaway downloads
// dir (never the real instance data).
//
// HOME is redirected alongside XDG_CONFIG_HOME, not because this file needs the
// settings writer to be safe, but because the orphan sweep DELETES through
// engine::remove_path() -> move_to_recycle_bin(), and that resolves
// XDG_DATA_HOME with a "$HOME/.local/share/Trash" fallback (linux_platform.cpp:
// trash_root()). One of the two env vars alone would put the real user's trash
// in the blast radius. Both are set, so the trash is part of the sandbox and
// the deletion this suite performs is provably confined to kRoot.
struct Sandbox {
  std::filesystem::path dl;
  std::filesystem::path trash;
  Sandbox() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const std::filesystem::path root(kRoot);
    const std::filesystem::path cfg = root / "config";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(cfg);
    qputenv("HOME", root.string().c_str());
    qputenv("XDG_CONFIG_HOME", cfg.string().c_str());
    qputenv("XDG_DATA_HOME", root.string().c_str());
    dl    = root / "dl";
    trash = root / "Trash";
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
      {"direct_url", a.direct_url, b.direct_url},
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

TEST_CASE("orphan meta files are swept, in-progress ones are kept", "[ui]") {
  Sandbox sandbox;
  int test_argc     = 1;
  char test_argv0[] = "download_meta_test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  sandbox.app(test_argc, test_argv);

  const std::filesystem::path dl = sandbox.dl;

  // 1. The orphan. No archive, no partial, a finished download - the archive
  //    was deleted from outside the app and the meta is all that is left.
  //    MO2 deletes exactly this (downloadmanager.cpp:409-422).
  const auto orphan = dl / "Orphan Mod-777-3-3.zip";
  ui::DownloadMeta orphan_meta;
  orphan_meta.archive       = orphan;
  orphan_meta.id            = "777-3";
  orphan_meta.name          = "Orphan Mod";
  orphan_meta.source        = "Nexus Mods";
  orphan_meta.parent_mod_id = "777";
  orphan_meta.file_id       = 3;
  orphan_meta.nexus_domain  = "skyrimspecialedition";
  orphan_meta.total_size    = 12345;
  orphan_meta.state         = ui::DownloadState::Complete;
  REQUIRE(
      ui::write_download_meta(orphan_meta, ui::download_meta_path(orphan_meta, dl)));

  // 1b. A second orphan, named so it sorts LAST. MO2 collects orphans into a
  //     QStringList and deletes them all in one pass, so "the first one works"
  //     is not the behaviour being adopted - every orphan in the dir goes.
  const auto orphan_last            = dl / "Zzz Orphan Mod-444-9-9.zip";
  ui::DownloadMeta orphan_last_meta = orphan_meta;
  orphan_last_meta.archive          = orphan_last;
  orphan_last_meta.id               = "444-9";
  orphan_last_meta.name             = "Zzz Orphan Mod";
  orphan_last_meta.file_id          = 9;
  REQUIRE(ui::write_download_meta(orphan_last_meta,
                                  ui::download_meta_path(orphan_last_meta, dl)));

  // 2. A meta whose archive IS on disk. Never a candidate, whatever its state.
  const auto present = dl / "Present Mod-888-4-4.zip";
  write_file(present, 4096);
  ui::DownloadMeta present_meta;
  present_meta.archive    = present;
  present_meta.id         = "888-4";
  present_meta.name       = "Present Mod";
  present_meta.state      = ui::DownloadState::Complete;
  present_meta.total_size = 4096;
  REQUIRE(
      ui::write_download_meta(present_meta, ui::download_meta_path(present_meta, dl)));

  // 3. A paused transfer with no completed archive. "paused" is how this comes
  //    back from disk, and it is the one state the sweep must not touch: the
  //    meta is the only record of what was being fetched.
  const auto paused = dl / "Paused Mod-999-5-5.zip";
  ui::DownloadMeta paused_meta;
  paused_meta.archive       = paused;
  paused_meta.id            = "999-5";
  paused_meta.name          = "Paused Mod";
  paused_meta.source        = "Nexus Mods";
  paused_meta.parent_mod_id = "999";
  paused_meta.file_id       = 5;
  paused_meta.total_size    = 999999;
  paused_meta.state         = ui::DownloadState::Paused;
  REQUIRE(
      ui::write_download_meta(paused_meta, ui::download_meta_path(paused_meta, dl)));

  // 4. An in-flight row written beside its partial, which is MO2's layout too
  //    (the ".unfinished" suffix, downloadmanager.cpp:111-114). No archive by
  //    design, so a filesystem-only sweep would take this one with it.
  const auto partial = dl / "Partial Mod-1212-6-6.zip.unfinished";
  write_file(partial, 512);
  ui::DownloadMeta partial_meta;
  partial_meta.archive       = std::filesystem::path{};
  partial_meta.partial_file  = partial.filename().string();
  partial_meta.id            = "1212-6";
  partial_meta.name          = "Partial Mod";
  partial_meta.source        = "Nexus Mods";
  partial_meta.parent_mod_id = "1212";
  partial_meta.file_id       = 6;
  partial_meta.total_size    = 40960;
  partial_meta.state         = ui::DownloadState::Downloading;
  REQUIRE(
      ui::write_download_meta(partial_meta, ui::download_meta_path(partial_meta, dl)));

  // 5. A SHORT archive - the P0 case. The file exists but is smaller than the
  //    recorded total_size, so archive_is_complete() rejects it downstream and
  //    deserialize() restores the row Failed. Its meta is what carries the
  //    total_size, so sweeping it would throw away the evidence.
  const auto short_zip = dl / "Short Mod-1313-7-7.zip";
  write_file(short_zip, 512);
  ui::DownloadMeta short_meta;
  short_meta.archive       = short_zip;
  short_meta.id            = "1313-7";
  short_meta.name          = "Short Mod";
  short_meta.source        = "Nexus Mods";
  short_meta.parent_mod_id = "1313";
  short_meta.file_id       = 7;
  short_meta.total_size    = 40960;
  short_meta.state         = ui::DownloadState::Complete;
  REQUIRE(ui::write_download_meta(short_meta, ui::download_meta_path(short_meta, dl)));

  // The sweep: exactly one meta disappears.
  const auto metas = ui::read_download_metas(dl);
  CHECK(std::filesystem::exists((dl / "Present Mod-888-4-4.zip.meta").string()));
  CHECK(std::filesystem::exists((dl / "Paused Mod-999-5-5.zip.meta").string()));
  CHECK(std::filesystem::exists(partial.string() + ".meta"));
  CHECK(std::filesystem::exists((dl / "Short Mod-1313-7-7.zip.meta").string()));
  CHECK_FALSE(std::filesystem::exists((dl / "Orphan Mod-777-3-3.zip.meta").string()));
  CHECK_FALSE(
      std::filesystem::exists((dl / "Zzz Orphan Mod-444-9-9.zip.meta").string()));

  // It went to the trash bin, MO2's shellDelete() behaviour, and into the
  // SANDBOX trash - which is the assertion that matters here: this suite
  // deletes files, and this is where they went. Both orphans, not just the
  // first one found.
  CHECK(
      std::filesystem::exists(sandbox.trash / "files" / "Orphan Mod-777-3-3.zip.meta"));
  CHECK(std::filesystem::exists(sandbox.trash / "files" /
                                "Zzz Orphan Mod-444-9-9.zip.meta"));

  REQUIRE(metas.size() == 4);
  for (const char *gone : {"777-3", "444-9"}) {
    const ui::DownloadMeta *found = find_meta(metas, gone);
    INFO("swept meta id: " << gone);
    CHECK(found == nullptr);
  }
  for (const char *kept : {"888-4", "999-5", "1212-6", "1313-7"}) {
    const ui::DownloadMeta *found = find_meta(metas, kept);
    INFO("kept meta id: " << kept);
    REQUIRE(found != nullptr);
  }

  // The kept identity fields are intact: the paused row is still the row it
  // was, so a fresh link can act on it instead of the list losing it. Its
  // state reads back Paused now, not Failed: the meta carries resumeState
  // ("paused" was written by a deliberate pause), and this row's identity
  // (domain + mod id + file id) is enough to rebuild the download link after
  // a restart - which is exactly what makes the row resumable instead of
  // dead. A crash mid-download writes no resumeState and still reads Failed.
  const ui::DownloadMeta *kept_paused = find_meta(metas, "999-5");
  REQUIRE(kept_paused != nullptr);
  CHECK(kept_paused->parent_mod_id == "999");
  CHECK(kept_paused->total_size == 999999);
  CHECK(kept_paused->state == ui::DownloadState::Paused);
  CHECK(kept_paused->archive == paused);
  // The in-flight row keeps its partial, which is what it resumes from.
  const ui::DownloadMeta *kept_partial = find_meta(metas, "1212-6");
  REQUIRE(kept_partial != nullptr);
  CHECK(kept_partial->partial_file == partial.filename().string());
  CHECK(kept_partial->archive.empty());

  // The P0 interaction: the short archive survives the sweep and comes back
  // Failed through the real restore path, because its meta still carries the
  // total_size the byte comparison needs.
  ui::DownloadsTab tab;
  tab.deserialize(ui::metas_to_manifest_json(metas, dl), dl);
  const ui::DownloadMeta *restored_short = find_meta(tab.to_metas(), "1313-7");
  REQUIRE(restored_short != nullptr);
  CHECK(restored_short->state == ui::DownloadState::Failed);

  // The two paths agree. scan_downloads_dir() drops vanished rows end-to-end
  // (downloads_tab.cpp:1040) and remove_entry() takes the meta with it, so
  // the orphan was never going to survive a full wiring - but it got there by
  // accident of call order, and only for FINISHED rows, and only when no
  // download was active (has_active_download() skips the scan). The sweep
  // removes it at the read instead, so it no longer depends on either.
  const auto orphan_meta_back = dl / "Orphan Mod-777-3-3.zip.meta";
  REQUIRE(
      ui::write_download_meta(orphan_meta, ui::download_meta_path(orphan_meta, dl)));
  REQUIRE(std::filesystem::exists(orphan_meta_back));
  ui::DownloadsTab agree;
  agree.deserialize(ui::metas_to_manifest_json(ui::read_download_metas(dl), dl), dl);
  CHECK_FALSE(std::filesystem::exists(orphan_meta_back));
  CHECK(find_meta(agree.to_metas(), "777-3") == nullptr);
  // And the scan must not take the rows the sweep kept. A paused row has a
  // file_path but no bytes, which is what it looks like when the app starts -
  // the scan used to read that as a deleted archive and dropped the row along
  // with its meta, which defeated the whole "keep in-progress work" rule.
  agree.set_downloads_dir(dl);
  CHECK(find_meta(agree.to_metas(), "999-5") != nullptr);
  CHECK(find_meta(agree.to_metas(), "1212-6") != nullptr);
  CHECK(std::filesystem::exists((dl / "Paused Mod-999-5-5.zip.meta").string()));
  CHECK(std::filesystem::exists(partial.string() + ".meta"));
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

// The instance-switch restart pauses in-flight downloads instead of
// cancelling them (Workspace-gb0b). The pause is only honest if the row
// actually comes back resumable: the tab records the direct URL a row was
// queued with, the meta persists it plus a resumeState marker that
// distinguishes a deliberate pause from a crash, and a FRESH tab (the
// process after the relaunch) restores the row Paused when its identity can
// rebuild the link, Failed when it cannot.
TEST_CASE("a download paused for a restart survives it resumable", "[ui]") {
  Sandbox sandbox;
  int test_argc     = 1;
  char test_argv0[] = "download_meta_test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  sandbox.app(test_argc, test_argv);
  const std::filesystem::path dl = sandbox.dl;

  ui::DownloadsTab tab;

  // The pause mechanics themselves: only rows whose transfer is actually in
  // flight are paused, and the request carries the row key.
  std::vector<std::string> pause_requests;
  QObject::connect(&tab, &ui::DownloadsTab::pause_requested,
                   [&pause_requests](const std::string &id) {
                     pause_requests.push_back(id);
                   });
  tab.add_download("32444-1234", "Nexus Mod", "Nexus Mods", {}, "skyrimspecialedition",
                   1234, "32444");
  // A real archive behind the finished row: the restore path verifies that a
  // Complete row's bytes are actually there.
  const auto ll_zip = dl / "LoversLab Mod-1.zip";
  write_file(ll_zip, 4096);
  tab.add_download("ll-1", "LoversLab Mod", "LoversLab", ll_zip, {}, 0, {},
                   "https://loverslab.com/files/file/1/");
  tab.note_direct_url("ll-1", "https://loverslab.com/files/file/1/?do=download");
  tab.mark_complete("ll-1", true);  // finished: nothing in flight to pause

  REQUIRE(tab.downloading_ids() == std::vector<std::string>{"32444-1234"});
  tab.pause_downloading();
  CHECK(pause_requests == std::vector<std::string>{"32444-1234"});

  // The row lands Paused (the worker's paused(id) -> mark_paused in
  // production; the state flip is what the restart waits for), and a paused
  // row is no longer "downloading".
  tab.mark_paused("32444-1234");
  CHECK(tab.downloading_ids().empty());

  // A paused row with no rebuildable identity and no direct URL cannot be
  // resumed after a restart - the link maps are in-memory only. It must come
  // back Failed, never wedged in Paused (has_active_download() counts
  // Paused, so a wedged row would stop the downloads-dir scan for the
  // whole session).
  tab.add_download("manual-1", "Manual Mod", "Manual", {}, {}, 0, {});
  tab.mark_paused("manual-1");

  // Persist through the real write path, then restore into a FRESH tab -
  // the process after the relaunch.
  tab.set_downloads_dir(dl);
  REQUIRE(ui::write_download_metas(tab.to_metas(), dl));

  ui::DownloadsTab restored;
  restored.deserialize(ui::metas_to_manifest_json(ui::read_download_metas(dl), dl), dl);
  const auto round_tripped = restored.to_metas();
  REQUIRE(round_tripped.size() == 3);

  // The Nexus row: identity (domain + mod id + file id) rebuilds the link,
  // so the deliberate pause survives as Paused - resumable.
  const ui::DownloadMeta *nexus = find_meta(round_tripped, "32444-1234");
  REQUIRE(nexus != nullptr);
  CHECK(nexus->state == ui::DownloadState::Paused);
  CHECK(nexus->nexus_domain == "skyrimspecialedition");
  CHECK(nexus->parent_mod_id == "32444");
  CHECK(nexus->file_id == 1234);

  // The LoversLab row: the direct URL was persisted, so it too is
  // resumable - and it is still Complete, because it was never paused.
  const ui::DownloadMeta *ll = find_meta(round_tripped, "ll-1");
  REQUIRE(ll != nullptr);
  CHECK(ll->state == ui::DownloadState::Complete);
  CHECK(ll->direct_url == "https://loverslab.com/files/file/1/?do=download");

  // The identity-less paused row: no link can be rebuilt, so Failed.
  const ui::DownloadMeta *manual = find_meta(round_tripped, "manual-1");
  REQUIRE(manual != nullptr);
  CHECK(manual->state == ui::DownloadState::Failed);
}