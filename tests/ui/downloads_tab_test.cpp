// Regression test for the Downloads tab's untracked-archive scan.
//
// The Downloads tab must list archives sitting in the instance downloads dir
// even when they are not tracked in .download_manifest.json (manually
// downloaded / dragged-in archives), so the user can double-click to install
// them. Verifies:
//   - set_downloads_dir() scans the dir and adds unknown archives as
//     "Manual" / "Install" (Complete) rows with the stem as display name,
//   - non-archive files, the manifest, and nested subdir archives are ignored,
//   - an archive that already backs a tracked entry is not duplicated,
//   - the scan is skipped while a download is Downloading/Paused (the partial
//     in-progress archive must not appear as a bogus Complete row) and runs
//     again once the download is done,
//   - double-clicking an untracked row emits install_requested with the real
//     archive path (source_type "", i.e. a local archive),
//   - dropping archives onto the tab moves/copies them into the downloads dir
//     per the proposed action, surfaces a Manual/Install row, and resolves
//     name conflicts via the injected resolver (MO2 parity: Overwrite,
//     N_<name> rename, Ignore),
//   - the directory watchdog refreshes the view on its own: a new archive
//     appears, an overwritten tracked archive's size updates, a deleted
//     archive's row is removed, and finishing the last active download ends
//     the scan guard so in-flight partials surface,
//   - the SAME watchdog and drop flows work when the tab starts EMPTY (a
//     fresh instance downloads dir) - a 0-row tab must still come alive,
//   - an add -> external delete -> add cycle on ONE tab keeps the tab alive:
//     removing every entry (rows disappear) must not leave a stale row counter
//     that breaks subsequent inserts (reported: "removed entries from the file
//     manager, rows disappeared, then any further change stopped showing up"),
//   - the row context menu is source-aware: a LoversLab row offers "Open on
//     LoversLab" (its stored page URL), a Nexus row "Open on Nexus" (domain +
//     mod id), and a local row no page action; triggering Install on a
//     LoversLab row carries the origin provenance (source_type loverslab, the
//     file id as source_id, and the page URL).
//
// Drag-event delivery: QApplication::notify routes Drag/Drop events to the
// active drag's current target only, so a synthesized QDropEvent never
// reaches dropEvent() (same limitation as plugins_tab_test.cpp). The handlers
// are therefore invoked directly through a subclass that exposes the
// protected overrides. proposedAction() is still derived correctly offscreen
// from the constructor modifiers (Shift -> MoveAction, Ctrl -> CopyAction).
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, no network.
#include "ui/panels/tab_panels.h"

#include <QApplication>
#include <QCheckBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QHeaderView>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QSet>
#include <QThread>
#include <QTreeWidget>
#include <QUrl>

#include "ui/settings/settings.h"
#include "ui/theme/icon_manager.h"

#include "engine/core/instance/instance.h"
#include "engine/core/log/logger.h"
#include "engine/game/registry/game_capabilities.h"
#include "engine/game/registry/game_knowledge.h"
#include "ui/controllers/settings_controller.h"
#include "ui/main_window/main_window.h"
#include "ui/widgets/column_toggle_header.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"
#include "ui/widgets/right_panel.h"

#include <QByteArray>

#include <algorithm>
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
}  // namespace

// Write a fake archive (any bytes will do - the tab only cares about the name
// and size).
static void write_file(const std::filesystem::path &path, size_t size) {
  std::ofstream out(path, std::ios::binary);
  out.write("x", 1);
  for (size_t i = 1; i < size; ++i)
    out.put(static_cast<char>('a' + (i % 26)));
}

// 1x1 transparent PNG - a real image so the vendor icon actually loads.
static const char *kPngB64 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42"
                             "mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";
static void write_png(const std::filesystem::path &p) {
  const QByteArray bytes = QByteArray::fromBase64(kPngB64);
  std::ofstream out(p, std::ios::binary);
  out.write(bytes.constData(), bytes.size());
}

// Row index whose Name column equals `name`, or -1.
static int row_with_name(QTableWidget *table, const char *name) {
  for (int r = 0; r < table->rowCount(); ++r) {
    auto *it = table->item(r, 0);
    if (it && it->text() == QLatin1String(name))
      return r;
  }
  return -1;
}

// Expose the protected drag/drop handlers so the drop flow can be driven
// directly (see the delivery caveat in the file header).
struct TestDownloadsTab : ui::DownloadsTab {
  using ui::DownloadsTab::add_context_menu_actions;
  using ui::DownloadsTab::dragEnterEvent;
  using ui::DownloadsTab::dragMoveEvent;
  using ui::DownloadsTab::dropEvent;
};

TEST_CASE("downloads tab", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_downloads_tab/config";
  std::filesystem::remove_all("/tmp/gmm_downloads_tab");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // Hermetic IconManager: a synthetic resources tree whose vendor/ dir ships
  // a branded Nexus icon, so the Source-cell icon below resolves for real.
  const std::filesystem::path icons = "/tmp/gmm_downloads_tab/icons";
  std::filesystem::create_directories(icons / "resources" / "icons" / "vendor");
  write_png(icons / "resources" / "icons" / "vendor" / "nexusmods.ico");
  engine::IconManager::instance().discover_packs(icons / "app");

  const std::filesystem::path dl_dir = "/tmp/gmm_downloads_tab/dl";
  std::filesystem::create_directories(dl_dir);

  // Untracked manual archive the scan should surface.
  const auto manual_zip = dl_dir / "My Mod.zip";
  write_file(manual_zip, 2048);
  // Archive that backs a tracked Nexus entry - must be deduped.
  const auto tracked_zip = dl_dir / "Tracked File-32444-11-1234.zip";
  write_file(tracked_zip, 4096);
  // Non-archive file, the manifest, a subdir with an archive: all ignored.
  write_file(dl_dir / "notes.txt", 100);
  write_file(dl_dir / ".download_manifest.json", 100);
  std::filesystem::create_directories(dl_dir / "sub");
  write_file(dl_dir / "sub" / "nested.zip", 100);

  TestDownloadsTab tab;

  // Pre-seed a tracked entry whose on-disk archive is tracked_zip (the key
  // is "<mod_id>-<file_id>", not the filename). Mark it complete first so
  // the scan is not blocked by the active-download guard.
  tab.add_download("32444-1234", "Tracked File", "Nexus Mods", tracked_zip,
                   "skyrimspecialedition", 1234, "32444");
  tab.mark_complete("32444-1234", true);

  // Branded source icon on the Source cell, resolved from the vendor dir.
  auto *nexus_src = tab.table()->item(0, 1);
  check(nexus_src && !nexus_src->icon().isNull(),
        "Nexus Mods row carries the branded vendor icon");

  // Trigger the scan via set_downloads_dir (what MainWindow does on startup).
  tab.set_downloads_dir(dl_dir);

  auto *table = tab.table();
  check(table->rowCount() == 2,
        "scan adds only the untracked archive (dedupes the tracked file)");

  // The scanned Manual row must NOT carry a vendor icon.
  auto *manual_src = table->item(1, 1);
  check(manual_src && manual_src->text() == QLatin1String("Manual") &&
            manual_src->icon().isNull(),
        "Manual row has no vendor icon");

  // Compact rows are sized explicitly (not by any stylesheet), so the
  // heights apply at launch even with no QSS installed.
  const int compact_h = qMax(24, table->fontMetrics().height() + 8);
  check(table->verticalHeader()->defaultSectionSize() == compact_h,
        "default section size equals the compact row height");
  for (int r = 0; r < table->rowCount(); ++r) {
    check(table->rowHeight(r) == compact_h,
          "scanned row uses the explicit compact height");
  }

  // Toggling the setting re-heights existing rows immediately.
  Settings::instance().set_compact_downloads(false);
  tab.apply_compact_style();
  const int standard_h = qMax(40, table->fontMetrics().height() + 22);
  check(table->verticalHeader()->defaultSectionSize() == standard_h,
        "default section size grows in standard mode");
  for (int r = 0; r < table->rowCount(); ++r) {
    check(table->rowHeight(r) == standard_h, "rows re-heigh on compact toggle");
  }
  Settings::instance().set_compact_downloads(true);
  tab.apply_compact_style();
  check(table->rowHeight(0) == compact_h, "rows shrink back after toggling compact on");

  const int manual_row = row_with_name(table, "My Mod");
  check(manual_row >= 0, "untracked archive listed with its stem as name");
  if (manual_row >= 0) {
    check(table->item(manual_row, 1)->text() == "Manual",
          "untracked row has Manual source");
    check(table->item(manual_row, 2)->text() == "Install",
          "untracked row shows Install (Complete) status");
    check(table->item(manual_row, 3)->text() == "2.0 KB",
          "untracked row shows the archive size");
    auto *status = table->item(manual_row, 2);
    check(status->foreground().color().name() == "#4caf50" &&
              status->background().style() == Qt::NoBrush,
          "Complete status: normal background, green Install text");
  }
  check(row_with_name(table, "Tracked File-32444-11-1234.zip") < 0,
        "archive backing a tracked entry is not duplicated");
  check(row_with_name(table, "notes.txt") < 0, "non-archive files are ignored");
  check(row_with_name(table, ".download_manifest.json") < 0,
        "the manifest file itself is ignored");
  check(row_with_name(table, "nested") < 0,
        "archives inside subdirectories are ignored");

  // A second archive dropped in later appears on the next scan.
  const auto later_zip = dl_dir / "Later Mod.7z";
  write_file(later_zip, 1024);
  tab.set_downloads_dir(dl_dir);
  check(row_with_name(table, "Later Mod") >= 0,
        "archive dropped in later is picked up by a re-scan");

  // Active-download guard: while an entry is Downloading the scan is
  // skipped, so the in-progress partial must not appear as Complete.
  tab.add_download("dl-1", "In flight", "Nexus Mods");
  const auto partial_zip = dl_dir / "Partial File.zip";
  write_file(partial_zip, 300);
  tab.set_downloads_dir(dl_dir);
  check(row_with_name(table, "Partial File") < 0,
        "scan skipped while a download is in flight (no bogus Complete row)");
  tab.mark_complete("dl-1", true);
  tab.set_downloads_dir(dl_dir);
  check(row_with_name(table, "Partial File") >= 0,
        "scan runs again once the download finishes");

  // Paused entries also hold the scan back.
  tab.add_download("dl-2", "Paused dl", "Nexus Mods");
  tab.mark_paused("dl-2");
  const auto more_zip = dl_dir / "More Mod.rar";
  write_file(more_zip, 200);
  tab.set_downloads_dir(dl_dir);
  check(row_with_name(table, "More Mod") < 0, "scan skipped while an entry is Paused");
  tab.mark_complete("dl-2", false);
  tab.set_downloads_dir(dl_dir);
  check(row_with_name(table, "More Mod") >= 0,
        "scan runs again after the paused entry resolves");
  {
    const int failed_row = row_with_name(table, "Paused dl");
    check(failed_row >= 0, "failed row present");
    if (failed_row >= 0) {
      auto *status = table->item(failed_row, 2);
      check(status && status->text() == "Failed" &&
                status->foreground().color() == Qt::white &&
                status->background().color().name() == "#f44336",
            "Failed status keeps the red fill with white text");
    }
  }

  // The reserved "Removed" state (not implemented yet) renders dark-yellow
  // text with a normal background when restored from a manifest.
  {
    const std::string removed_json =
        "[{\"id\":\"removed-1\",\"name\":\"RemovedMod\",\"source\":\"Manual\","
        "\"file_path\":\"" +
        manual_zip.string() + "\",\"state\":5,\"total_size\":0}]";
    tab.deserialize(removed_json, dl_dir);
    const int removed_row = row_with_name(table, "RemovedMod");
    check(removed_row >= 0, "Removed-state entry restored from manifest");
    if (removed_row >= 0) {
      auto *status = table->item(removed_row, 2);
      check(status && status->text() == "Removed" &&
                status->foreground().color().name() == "#b8860b" &&
                status->background().style() == Qt::NoBrush,
            "Removed status renders dark-yellow text on normal background");
    }
  }

  // Double-clicking the untracked row emits install_requested with the real
  // archive path and an empty source type (local archive install).
  bool got_install            = false;
  std::string got_source_type = "unset";
  std::filesystem::path got_path;
  QObject::connect(&tab, &ui::DownloadsTab::install_requested,
                   [&](const std::string &, const std::filesystem::path &fp,
                       const std::string &source_type, const std::string &, int,
                       const std::string &name, const std::string &) {
                     got_install     = true;
                     got_path        = fp;
                     got_source_type = source_type;
                     (void)name;
                   });
  const int my_row = row_with_name(table, "My Mod");
  check(my_row >= 0, "untracked row present before double-click");
  if (my_row >= 0) {
    QMetaObject::invokeMethod(table, "cellDoubleClicked", Qt::DirectConnection,
                              Q_ARG(int, my_row), Q_ARG(int, 0));
    app.processEvents();
    check(got_install && got_path == manual_zip && got_source_type.empty(),
          "double-click on untracked row emits install_requested (local)");
  }

  // --- External archive drops (MO2 downloads-tab parity) ---
  const std::filesystem::path src_dir = "/tmp/gmm_downloads_tab/src";
  std::filesystem::create_directories(src_dir);

  // Build a drop carrying a single local file URL. move=true proposes
  // MoveAction (Shift drag), move=false proposes CopyAction (Ctrl drag) -
  // matching how Qt derives the proposed action from the modifiers.
  auto send_drop = [&](TestDownloadsTab *target, const std::filesystem::path &file,
                       bool move, bool *accepted = nullptr,
                       Qt::DropAction *drop_action = nullptr) {
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QString::fromStdString(file.string()))});
    QDropEvent event(QPointF(5, 5), Qt::MoveAction | Qt::CopyAction, &mime,
                     Qt::LeftButton, move ? Qt::ShiftModifier : Qt::ControlModifier);
    target->dropEvent(&event);
    if (accepted)
      *accepted = event.isAccepted();
    if (drop_action)
      *drop_action = event.dropAction();
  };

  // Drag gate: only local archive files are accepted (and then with the
  // proposed action, as Qt dictates for the hand-off).
  {
    QMimeData gate_mime;
    gate_mime.setUrls({QUrl::fromLocalFile(
        QString::fromStdString((src_dir / "Gate Mod.zip").string()))});
    QDragEnterEvent gate(QPoint(5, 5), Qt::MoveAction | Qt::CopyAction, &gate_mime,
                         Qt::LeftButton, Qt::ShiftModifier);
    tab.dragEnterEvent(&gate);
    check(gate.isAccepted(), "dragEnterEvent accepts a local archive drop");
    QMimeData bad_mime;
    bad_mime.setUrls({QUrl::fromLocalFile(
        QString::fromStdString((src_dir / "Readme.txt").string()))});
    QDragEnterEvent bad(QPoint(5, 5), Qt::MoveAction | Qt::CopyAction, &bad_mime,
                        Qt::LeftButton, Qt::ShiftModifier);
    tab.dragEnterEvent(&bad);
    check(!bad.isAccepted(), "dragEnterEvent ignores a non-archive drop");
    QMimeData http_mime;
    http_mime.setUrls({QUrl(QStringLiteral("https://example.com/x.zip"))});
    QDragEnterEvent http(QPoint(5, 5), Qt::MoveAction | Qt::CopyAction, &http_mime,
                         Qt::LeftButton, Qt::ShiftModifier);
    tab.dragEnterEvent(&http);
    check(!http.isAccepted(), "dragEnterEvent ignores remote URLs");
  }

  // A LoversLab download link dropped from a browser routes through the same
  // "Add from URL…" flow (loverslab_url_entered); a non-LoversLab remote URL
  // is still rejected and archive drops keep the file-import path.
  {
    std::string dropped_url;
    QObject::connect(&tab, &ui::DownloadsTab::loverslab_url_entered,
                     [&dropped_url](const std::string &url) {
                       dropped_url = url;
                     });

    QMimeData ll_mime;
    ll_mime.setUrls({QUrl(QStringLiteral(
        "https://www.loverslab.com/files/file/10093-slug/?do=download&r=7"))});
    QDragEnterEvent ll_enter(QPoint(5, 5), Qt::MoveAction | Qt::CopyAction, &ll_mime,
                             Qt::LeftButton, Qt::ShiftModifier);
    tab.dragEnterEvent(&ll_enter);
    check(ll_enter.isAccepted(), "dragEnterEvent accepts a LoversLab URL drop");

    QDropEvent ll_drop(QPointF(5, 5), Qt::MoveAction | Qt::CopyAction, &ll_mime,
                       Qt::LeftButton, Qt::ShiftModifier);
    tab.dropEvent(&ll_drop);
    check(ll_drop.isAccepted(), "LoversLab URL drop is accepted");
    check(dropped_url ==
              "https://www.loverslab.com/files/file/10093-slug/?do=download&r=7",
          "dropped LoversLab URL emitted via loverslab_url_entered");

    // A LoversLab page link dropped as bare text is also routed.
    QMimeData ll_text;
    ll_text.setText(QStringLiteral("https://www.loverslab.com/files/file/200/"));
    QDropEvent text_drop(QPointF(5, 5), Qt::MoveAction | Qt::CopyAction, &ll_text,
                         Qt::LeftButton, Qt::ShiftModifier);
    tab.dropEvent(&text_drop);
    check(dropped_url == "https://www.loverslab.com/files/file/200/",
          "text-only LoversLab URL drop also routed");

    // A multi-URL drag is not a single download link: stays rejected.
    QMimeData multi_mime;
    multi_mime.setUrls(
        {QUrl(QStringLiteral("https://www.loverslab.com/files/file/1/")),
         QUrl(QStringLiteral("https://www.loverslab.com/files/file/2/"))});
    QDragEnterEvent multi_enter(QPoint(5, 5), Qt::MoveAction | Qt::CopyAction,
                                &multi_mime, Qt::LeftButton, Qt::ShiftModifier);
    tab.dragEnterEvent(&multi_enter);
    check(!multi_enter.isAccepted(), "multi-URL drag stays rejected");
  }

  const auto moved_src = src_dir / "Dropped Mod.zip";
  write_file(moved_src, 1024);
  bool moved_accepted         = false;
  Qt::DropAction moved_action = Qt::IgnoreAction;
  send_drop(&tab, moved_src, true, &moved_accepted, &moved_action);
  check(moved_accepted, "move drop is accepted");
  check(moved_action == Qt::TargetMoveAction,
        "move drop takes the source (TargetMoveAction, MO2 parity)");
  check(!std::filesystem::exists(moved_src), "move drop removes the source archive");
  check(std::filesystem::exists(dl_dir / "Dropped Mod.zip"),
        "move drop lands the archive in the downloads dir");
  const int dropped_row = row_with_name(table, "Dropped Mod");
  check(dropped_row >= 0, "move drop surfaces a Manual row immediately");
  if (dropped_row >= 0) {
    check(table->item(dropped_row, 1)->text() == "Manual" &&
              table->item(dropped_row, 2)->text() == "Install",
          "dropped row is Manual/Install (Complete)");
  }

  const auto copied_src = src_dir / "Copied Mod.7z";
  write_file(copied_src, 512);
  bool copied_accepted         = false;
  Qt::DropAction copied_action = Qt::IgnoreAction;
  send_drop(&tab, copied_src, false, &copied_accepted, &copied_action);
  check(copied_accepted, "copy drop is accepted");
  check(copied_action == Qt::CopyAction,
        "copy drop leaves the source in place (CopyAction)");
  check(std::filesystem::exists(copied_src), "copy drop keeps the source archive");
  check(std::filesystem::exists(dl_dir / "Copied Mod.7z"),
        "copy drop lands a copy in the downloads dir");
  check(row_with_name(table, "Copied Mod") >= 0, "copy drop surfaces a row too");

  const auto notes = src_dir / "Readme.txt";
  write_file(notes, 64);
  bool notes_accepted = true;
  send_drop(&tab, notes, true, &notes_accepted);
  check(!notes_accepted, "non-archive drop is ignored");
  check(std::filesystem::exists(notes) &&
            !std::filesystem::exists(dl_dir / "Readme.txt") &&
            row_with_name(table, "Readme") < 0,
        "non-archive drop is rejected (nothing moved, no row)");

  // Dropping a file that already lives in the downloads dir is a no-op for
  // the file operation: the entry is surfaced, the file is not clobbered.
  const auto self_file = dl_dir / "Self Drop.zip";
  write_file(self_file, 256);
  send_drop(&tab, self_file, true);
  check(std::filesystem::exists(self_file),
        "drop of an already-present file does not clobber it");
  check(row_with_name(table, "Self Drop") >= 0,
        "drop of an already-present file surfaces its row");

  // Name conflict with an existing archive: inject resolvers so no modal
  // appears. Rename -> MO2-style N_<name> numbering.
  const auto clash_src  = src_dir / "Clash Mod.zip";
  const auto clash_dest = dl_dir / "Clash Mod.zip";
  write_file(clash_src, 512);
  write_file(clash_dest, 64);
  tab.set_conflict_resolver(
      [](const std::filesystem::path &, const std::filesystem::path &) {
        return ui::DropConflictAction::Rename;
      });
  send_drop(&tab, clash_src, true);
  check(std::filesystem::exists(dl_dir / "1_Clash Mod.zip"),
        "conflict Rename lands the archive as 1_<name>");
  check(!std::filesystem::exists(clash_src), "conflict Rename still moves the source");
  check(row_with_name(table, "1_Clash Mod") >= 0,
        "conflict Rename row shown under the numbered name");

  // Conflict -> Ignore: nothing is moved, no new entry.
  const auto ignore_src  = src_dir / "Ignore Mod.zip";
  const auto ignore_dest = dl_dir / "Ignore Mod.zip";
  write_file(ignore_src, 512);
  write_file(ignore_dest, 64);
  tab.set_conflict_resolver(
      [](const std::filesystem::path &, const std::filesystem::path &) {
        return ui::DropConflictAction::Ignore;
      });
  send_drop(&tab, ignore_src, true);
  check(std::filesystem::exists(ignore_src),
        "conflict Ignore leaves the source in place");
  check(row_with_name(table, "Ignore Mod") < 0, "conflict Ignore adds no row");

  // Conflict -> Overwrite: the existing archive is replaced by the drop.
  const auto over_src  = src_dir / "Overwrite Mod.zip";
  const auto over_dest = dl_dir / "Overwrite Mod.zip";
  {
    std::ofstream out(over_src, std::ios::binary);
    out << "new content";
  }
  {
    std::ofstream out(over_dest, std::ios::binary);
    out << "old content";
  }
  tab.set_conflict_resolver(
      [](const std::filesystem::path &, const std::filesystem::path &) {
        return ui::DropConflictAction::Overwrite;
      });
  send_drop(&tab, over_src, true);
  check(std::filesystem::exists(over_dest),
        "conflict Overwrite keeps the destination name");
  {
    std::ifstream in(over_dest, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    check(content == "new content",
          "conflict Overwrite replaces the existing archive contents");
  }

  // Overwrite of an ALREADY-TRACKED entry (the real "drop the same name
  // again" flow): the row must refresh with the new size, not duplicate.
  // Move branch (Shift).
  const auto re_src  = src_dir / "Replaced Mod.zip";
  const auto re_dest = dl_dir / "Replaced Mod.zip";
  write_file(re_dest, 64);
  tab.set_downloads_dir(dl_dir);
  const int re_seed_row = row_with_name(table, "Replaced Mod");
  check(re_seed_row >= 0, "existing archive is tracked before the overwrite drop");
  if (re_seed_row >= 0)
    check(table->item(re_seed_row, 3)->text() == "64 B",
          "tracked row initially shows the old size");
  write_file(re_src, 512);
  send_drop(&tab, re_src, true);
  check(!std::filesystem::exists(re_src),
        "overwrite of a tracked entry moves the source");
  check(std::filesystem::exists(re_dest),
        "overwrite of a tracked entry replaces the archive on disk");
  const int re_row = row_with_name(table, "Replaced Mod");
  check(re_row >= 0 && re_row == re_seed_row,
        "overwrite of a tracked entry refreshes the row (no duplicate)");
  if (re_row >= 0)
    check(table->item(re_row, 3)->text() == "512 B",
          "overwrite of a tracked entry updates the size immediately");

  // Same flow on the copy branch (Ctrl): the source stays, the tracked row
  // still refreshes without duplicating.
  const auto re_copy_src  = src_dir / "Replaced Copy.zip";
  const auto re_copy_dest = dl_dir / "Replaced Copy.zip";
  write_file(re_copy_dest, 64);
  tab.set_downloads_dir(dl_dir);
  const int rc_seed_row = row_with_name(table, "Replaced Copy");
  check(rc_seed_row >= 0, "existing archive is tracked before the copy overwrite");
  write_file(re_copy_src, 1024);
  send_drop(&tab, re_copy_src, false);
  check(std::filesystem::exists(re_copy_src),
        "copy overwrite of a tracked entry keeps the source");
  check(std::filesystem::exists(re_copy_dest),
        "copy overwrite of a tracked entry updates the destination");
  const int rc_row = row_with_name(table, "Replaced Copy");
  check(rc_row >= 0 && rc_row == rc_seed_row,
        "copy overwrite of a tracked entry refreshes the row (no duplicate)");
  if (rc_row >= 0)
    check(table->item(rc_row, 3)->text() == "1.0 KB",
          "copy overwrite of a tracked entry updates the size immediately");

  // --- Downloads-dir watchdog: external changes surface on their own (a
  // file-manager drop/delete must update the tab without a manual scan).
  // The watcher + debounce timer need a live event loop, so poll with real
  // sleeps until the expectation holds or the deadline passes.
  auto wait_until = [&app](int timeout_ms, const std::function<bool()> &cond) {
    for (int waited = 0; waited < timeout_ms; waited += 10) {
      app.processEvents();
      if (cond())
        return true;
      QThread::msleep(10);
    }
    return false;
  };

  TestDownloadsTab watch_tab;
  watch_tab.set_downloads_dir(dl_dir);

  // 1) A new archive landing in the dir appears without a manual scan.
  const auto watched_add = dl_dir / "Watched Add.zip";
  write_file(watched_add, 2048);
  check(wait_until(1500,
                   [&]() {
                     return row_with_name(watch_tab.table(), "Watched Add") >= 0;
                   }),
        "watchdog surfaces a newly copied archive without a manual scan");
  const int wa_row = row_with_name(watch_tab.table(), "Watched Add");
  if (wa_row >= 0)
    check(watch_tab.table()->item(wa_row, 3)->text() == "2.0 KB",
          "watchdog row shows the new archive size");

  // 2) Replacing an already-tracked archive refreshes its size on its own.
  const auto watched_over = dl_dir / "Watched Overwrite.zip";
  write_file(watched_over, 64);
  check(wait_until(1500,
                   [&]() {
                     return row_with_name(watch_tab.table(), "Watched Overwrite") >= 0;
                   }),
        "watchdog surfaces the initial tracked archive");
  write_file(watched_over, 512);
  check(wait_until(1500,
                   [&]() {
                     const int r =
                         row_with_name(watch_tab.table(), "Watched Overwrite");
                     return r >= 0 && watch_tab.table()->item(r, 3)->text() == "512 B";
                   }),
        "watchdog refreshes the size of an overwritten tracked archive");

  // 3) Deleting an archive removes its row on its own.
  std::filesystem::remove(watched_over);
  check(wait_until(1500,
                   [&]() {
                     return row_with_name(watch_tab.table(), "Watched Overwrite") < 0;
                   }),
        "watchdog removes the row of a deleted archive");

  // 4) Finishing the last active download ends the scan guard and surfaces a
  // partial archive that landed while the download was in flight.
  TestDownloadsTab guard_tab;
  guard_tab.add_download("watch-dl", "Watch Flight", "Nexus Mods");
  const auto watched_partial = dl_dir / "Watched Partial.zip";
  write_file(watched_partial, 300);
  guard_tab.set_downloads_dir(dl_dir);
  check(row_with_name(guard_tab.table(), "Watched Partial") < 0,
        "watchdog scan still skips while a download is in flight");
  guard_tab.mark_complete("watch-dl", true);
  check(wait_until(1500,
                   [&]() {
                     return row_with_name(guard_tab.table(), "Watched Partial") >= 0;
                   }),
        "finishing the last download auto-scans and surfaces the archive");

  // --- Empty-tab regression: a fresh, EMPTY downloads tab must still come
  // alive when files land in its dir (watchdog) or are dropped onto it. The
  // pre-seeded dirs above never covered this state (reported: a 0-row tab
  // fails to ever update, no matter how files end up in the downloads dir).
  const std::filesystem::path empty_dir = "/tmp/gmm_downloads_tab/empty";
  std::filesystem::create_directories(empty_dir);

  TestDownloadsTab empty_tab;
  empty_tab.set_downloads_dir(empty_dir);
  check(empty_tab.table()->rowCount() == 0,
        "empty downloads tab starts with zero rows");

  // 1) Watchdog: a file copied into the previously-empty dir surfaces.
  const auto fresh_zip = empty_dir / "Fresh Mod.zip";
  write_file(fresh_zip, 1024);
  check(wait_until(1500,
                   [&]() {
                     return row_with_name(empty_tab.table(), "Fresh Mod") >= 0;
                   }),
        "empty tab: watchdog surfaces a file copied into the dir");

  // 2) Drop onto the empty tab: the archive lands in the dir AND a row
  // appears (the reported flow is drag into a 0-row tab).
  const auto empty_drop_src = src_dir / "Empty Tab Drop.zip";
  write_file(empty_drop_src, 512);
  bool empty_drop_accepted = false;
  send_drop(&empty_tab, empty_drop_src, true, &empty_drop_accepted);
  check(empty_drop_accepted, "empty tab: drop is accepted");
  check(std::filesystem::exists(empty_dir / "Empty Tab Drop.zip"),
        "empty tab: drop lands the archive in the downloads dir");
  check(row_with_name(empty_tab.table(), "Empty Tab Drop") >= 0,
        "empty tab: dropped archive surfaces a row immediately");

  // --- Add/remove/add regression: the reported break is "added entries to
  // an empty list (worked), removed them manually from a file manager (rows
  // disappeared), then ANY further change stopped showing up". The watchdog
  // removal path must leave the tab alive for the next addition.
  const std::filesystem::path cycle_dir = "/tmp/gmm_downloads_tab/cycle";
  std::filesystem::create_directories(cycle_dir);

  TestDownloadsTab cycle_tab;
  cycle_tab.set_downloads_dir(cycle_dir);
  check(cycle_tab.table()->rowCount() == 0, "add/remove/add: cycle tab starts empty");

  const auto cycle_a = cycle_dir / "Cycle A.zip";
  write_file(cycle_a, 128);
  check(wait_until(1500,
                   [&]() {
                     return row_with_name(cycle_tab.table(), "Cycle A") >= 0;
                   }),
        "add/remove/add: first archive surfaces (add works)");

  std::filesystem::remove(cycle_a);
  check(wait_until(1500,
                   [&]() {
                     return row_with_name(cycle_tab.table(), "Cycle A") < 0;
                   }),
        "add/remove/add: external delete removes the row");

  const auto cycle_b = cycle_dir / "Cycle B.zip";
  write_file(cycle_b, 256);
  check(wait_until(1500,
                   [&]() {
                     return row_with_name(cycle_tab.table(), "Cycle B") >= 0;
                   }),
        "add/remove/add: a new archive still surfaces after a removal");

  const auto cycle_c = cycle_dir / "Cycle C.zip";
  write_file(cycle_c, 512);
  check(wait_until(1500,
                   [&]() {
                     return row_with_name(cycle_tab.table(), "Cycle C") >= 0;
                   }),
        "add/remove/add: the tab stays alive for further additions");

  // --- Source-aware "Open on ..." context action + install provenance ---
  // The context menu is driven through add_context_menu_actions (no modal
  // exec): a LoversLab row offers "Open on LoversLab" opening its stored
  // page URL, a Nexus row "Open on Nexus" built from domain + mod id, and a
  // local row no page action at all. Triggering a LoversLab row's Install
  // must carry the origin provenance (source_type, file id, page URL).
  TestDownloadsTab ctx_tab;

  auto find_action = [](QMenu &menu, const char *text) -> QAction * {
    const QString needle = QString::fromLatin1(text);
    for (auto *act : menu.actions())
      if (act->text() == needle)
        return act;
    return nullptr;
  };

  // Complete LoversLab entry with a real archive on disk.
  const std::string ll_page =
      "https://www.loverslab.com/files/file/4242-skooma-whore-se/";
  const auto ll_zip = dl_dir / "Skooma Whore SE v1.01.zip";
  write_file(ll_zip, 512);
  ctx_tab.add_download("4242", "Skooma Whore SE v1.01", "LoversLab", ll_zip, {}, 0, {},
                       ll_page);
  ctx_tab.mark_complete("4242", true);
  {
    std::string got_st = "unset", got_sid = "unset", got_page = "unset";
    bool triggered = false;
    QObject::connect(&ctx_tab, &ui::DownloadsTab::install_requested,
                     [&](const std::string &, const std::filesystem::path &,
                         const std::string &st, const std::string &sid, int,
                         const std::string &, const std::string &page) {
                       triggered = true;
                       got_st    = st;
                       got_sid   = sid;
                       got_page  = page;
                     });
    QMenu menu;
    ctx_tab.add_context_menu_actions(menu, "4242");
    auto *act = find_action(menu, "Open on LoversLab");
    check(act && act->isEnabled() && act->data().toString().toStdString() == ll_page,
          "context menu: LoversLab row offers 'Open on LoversLab' with the page URL");
    check(!find_action(menu, "Open on Nexus"),
          "context menu: LoversLab row has no 'Open on Nexus' action");
    auto *install = find_action(menu, "Install");
    if (install)
      install->trigger();
    check(
        triggered && got_st == "loverslab" && got_sid == "4242" && got_page == ll_page,
        "context menu: LoversLab Install carries loverslab provenance (id + page URL)");
  }

  // Nexus entry: URL built from domain + parent mod id.
  ctx_tab.add_download("32444-1234", "Tracked File", "Nexus Mods", tracked_zip,
                       "skyrimspecialedition", 1234, "32444");
  {
    QMenu menu;
    ctx_tab.add_context_menu_actions(menu, "32444-1234");
    auto *act = find_action(menu, "Open on Nexus");
    check(act && act->isEnabled() &&
              act->data().toString().toStdString() ==
                  "https://www.nexusmods.com/skyrimspecialedition/mods/32444",
          "context menu: Nexus row offers 'Open on Nexus' with domain+modid URL");
    check(!find_action(menu, "Open on LoversLab"),
          "context menu: Nexus row has no 'Open on LoversLab' action");
  }

  // Local/manual entry: no page action at all.
  ctx_tab.add_download("manual-1", "My Mod", "Manual", manual_zip);
  {
    QMenu menu;
    ctx_tab.add_context_menu_actions(menu, "manual-1");
    check(!find_action(menu, "Open on Nexus") &&
              !find_action(menu, "Open on LoversLab"),
          "context menu: Manual row gets no page action");
  }

  // --- Filter regression: text filter + "hide installed" must compose ---
  // RightPanel::apply_filter applies the bar text via apply_to(table) and
  // then re-applies the hide-installed checkbox via
  // set_filter_text + reapply_installed_filter. Before the fix,
  // apply_installed_filter unhid every row the text filter had hidden, so
  // the Downloads "Filter..." bar did nothing (user report).
  {
    TestDownloadsTab f_tab;
    f_tab.add_download("t1", "Tracked File", "Nexus Mods", tracked_zip,
                       "skyrimspecialedition", 1234, "32444");
    f_tab.mark_complete("t1", true);
    f_tab.add_download("t2", "Skooma Whore SE", "LoversLab", ll_zip, {}, 0, {},
                       ll_page);
    f_tab.mark_complete("t2", true);
    auto *f_tab_tbl = f_tab.table();
    auto row_of     = [&](const char *name) {
      for (int r = 0; r < f_tab_tbl->rowCount(); ++r) {
        auto *item = f_tab_tbl->item(r, 0);
        if (item && item->text() == QLatin1String(name))
          return r;
      }
      return -1;
    };
    // The tab's only QCheckBox is the "Hide installed" toggle; checking it
    // fires the same toggled handler the UI uses.
    auto *hide_cb = f_tab.findChild<QCheckBox *>();
    check(hide_cb, "DownloadsTab exposes the hide-installed checkbox");

    // Text filter alone hides non-matching rows.
    f_tab.set_filter_text(QStringLiteral("Skooma"));
    f_tab.reapply_installed_filter();
    check(f_tab_tbl->isRowHidden(row_of("Tracked File")),
          "text filter hides non-matching row");
    check(!f_tab_tbl->isRowHidden(row_of("Skooma Whore SE")),
          "text filter keeps matching row");

    // Empty text restores all rows.
    f_tab.set_filter_text(QStringLiteral(""));
    f_tab.reapply_installed_filter();
    check(!f_tab_tbl->isRowHidden(row_of("Tracked File")) &&
              !f_tab_tbl->isRowHidden(row_of("Skooma Whore SE")),
          "clearing the text filter restores every row");

    // "Hide installed" + text: rows hidden by EITHER filter stay hidden,
    // matching what RightPanel::apply_filter composes.
    if (hide_cb)
      hide_cb->setChecked(true);
    f_tab.mark_installed("t1");
    f_tab.set_filter_text(QStringLiteral("Skooma"));
    f_tab.reapply_installed_filter();
    check(f_tab_tbl->isRowHidden(row_of("Tracked File")),
          "installed row stays hidden under text+hide-installed");
    check(!f_tab_tbl->isRowHidden(row_of("Skooma Whore SE")),
          "non-installed matching row visible under text+hide-installed");

    f_tab.set_filter_text(QStringLiteral(""));
    f_tab.reapply_installed_filter();
    check(f_tab_tbl->isRowHidden(row_of("Tracked File")),
          "hide-installed alone hides installed row");
    check(!f_tab_tbl->isRowHidden(row_of("Skooma Whore SE")),
          "hide-installed alone keeps uninstalled row");
  }
}

// Source-attribution regression for the Downloads tab (Workspace-rvld).
//
// source_info_for() used to trust the Source column's literal string
// alone. A bare "Nexus Mods" label with no parent_mod_id / no
// nexus_domain / no file_id was still treated as a Nexus install,
// which the install path then propagated as a fake Nexus modid in
// mods/{folder}/meta.ini and a [Nexusmods] section. Same shape for
// LoversLab: a row labelled "LoversLab" with no page_url / no id was
// silently treated as LoversLab-attributable. The fix only returns a
// source_type when ALL the fields needed to actually re-fetch the
// file are present; otherwise it returns empty (treated as Manual
// local archive). deserialize() also repairs legacy manifests that
// carried the bare labels without origin fields.
//
// The install_requested signal is the public observable that carries
// the source_type out of the tab into the pipeline worker, so we
// drive double-click flows and assert the emitted source_type /
// source_id / page_url. Internal source_info_for() is private and
// only ever runs as a step inside the double-click handler, so the
// emitted signal is a faithful witness of its return value.
TEST_CASE("downloads source attribution", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_source_attr/config";
  std::filesystem::remove_all("/tmp/gmm_source_attr");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const std::filesystem::path dl_dir = "/tmp/gmm_source_attr/dl";
  std::filesystem::create_directories(dl_dir);

  // Drive a double-click on the row with the given name and return
  // the install_requested fields the tab emitted.
  struct Captured {
    bool fired = false;
    std::string source_type;
    std::string source_id;
    int file_id = 0;
    std::string page_url;
  };
  auto double_click_and_capture = [&](TestDownloadsTab &tab,
                                      const char *name) -> Captured {
    Captured c;
    QObject::connect(&tab, &ui::DownloadsTab::install_requested,
                     [&](const std::string &, const std::filesystem::path &,
                         const std::string &st, const std::string &sid, int fid,
                         const std::string &, const std::string &page) {
                       c.fired       = true;
                       c.source_type = st;
                       c.source_id   = sid;
                       c.file_id     = fid;
                       c.page_url    = page;
                     });
    int row = -1;
    for (int r = 0; r < tab.table()->rowCount(); ++r) {
      auto *it = tab.table()->item(r, 0);
      if (it && it->text() == QLatin1String(name))
        row = r;
    }
    if (row < 0)
      return c;
    QMetaObject::invokeMethod(tab.table(), "cellDoubleClicked", Qt::DirectConnection,
                              Q_ARG(int, row), Q_ARG(int, 0));
    app.processEvents();
    return c;
  };

  // --- A complete Nexus row (id, file_id, domain all present) is
  // attributed to Nexus. ---
  {
    const auto zip = dl_dir / "Nexus Complete.zip";
    write_file(zip, 256);
    TestDownloadsTab tab;
    tab.add_download("32444-1234", "Nexus Complete", "Nexus Mods", zip,
                     "skyrimspecialedition", 1234, "32444");
    tab.mark_complete("32444-1234", true);
    const auto c = double_click_and_capture(tab, "Nexus Complete");
    check(c.fired && c.source_type == "nexus" && c.source_id == "32444" &&
              c.file_id == 1234,
          "complete Nexus row: install carries nexus+id+file_id");
  }

  // --- A Nexus-labelled row with NO origin fields is treated as
  // local (no source). This is the exact shape stale manifests used
  // to carry, and it was the source of the auto-Nexus injection. ---
  {
    const auto zip = dl_dir / "Nexus Stale.zip";
    write_file(zip, 256);
    TestDownloadsTab tab;
    // Label says "Nexus Mods" but no domain / parent_mod_id / file_id.
    tab.add_download("stale-nexus-1", "Nexus Stale", "Nexus Mods", zip);
    tab.mark_complete("stale-nexus-1", true);
    const auto c = double_click_and_capture(tab, "Nexus Stale");
    check(c.fired && c.source_type.empty() && c.source_id.empty() && c.file_id == 0,
          "stale Nexus label with no origin: install is manual "
          "(no fabricated Nexus)");
  }

  // --- A LoversLab row with a page_url and an id is attributed to
  // LoversLab. ---
  {
    const auto zip = dl_dir / "LoversLab Complete.zip";
    write_file(zip, 256);
    const std::string ll_page = "https://www.loverslab.com/files/file/4242-skooma-se/";
    TestDownloadsTab tab;
    tab.add_download("ll-4242", "LoversLab Complete", "LoversLab", zip, {}, 0, {},
                     ll_page);
    tab.mark_complete("ll-4242", true);
    const auto c = double_click_and_capture(tab, "LoversLab Complete");
    check(c.fired && c.source_type == "loverslab" && c.source_id == "ll-4242" &&
              c.page_url == ll_page,
          "complete LoversLab row: install carries loverslab+id+page");
  }

  // --- A LoversLab-labelled row with no page_url / no id is treated
  // as local. ---
  {
    const auto zip = dl_dir / "LoversLab Stale.zip";
    write_file(zip, 256);
    TestDownloadsTab tab;
    tab.add_download("stale-ll-1", "LoversLab Stale", "LoversLab", zip);
    tab.mark_complete("stale-ll-1", true);
    const auto c = double_click_and_capture(tab, "LoversLab Stale");
    check(c.fired && c.source_type.empty(),
          "stale LoversLab label with no page_url/id: install is manual");
  }

  // --- A Manual row stays empty. ---
  {
    const auto zip = dl_dir / "Manual.zip";
    write_file(zip, 256);
    TestDownloadsTab tab;
    tab.add_download("manual-1", "Manual", "Manual", zip);
    tab.mark_complete("manual-1", true);
    const auto c = double_click_and_capture(tab, "Manual");
    check(c.fired && c.source_type.empty(),
          "manual row: install is manual (empty source_type)");
  }

  // --- Manifest repair: a deserialized entry with source="Nexus Mods"
  // but no origin fields is coerced to "Manual" on the row, and
  // double-clicking it emits a manual install (no fabricated Nexus). ---
  {
    const auto zip = dl_dir / "StaleNexus.zip";
    write_file(zip, 256);
    TestDownloadsTab tab;
    const std::string stale_json =
        "[{\"id\":\"stale-1\",\"name\":\"StaleNexus\",\"source\":\"Nexus Mods\","
        "\"file_path\":\"" +
        zip.string() +
        "\","
        "\"state\":2,\"total_size\":0,\"parent_mod_id\":\"\","
        "\"file_id\":0,\"domain\":\"\",\"category\":\"\",\"page_url\":\"\"}]";
    tab.deserialize(stale_json, dl_dir);

    // Row surfaced under the original name, with the coerced label.
    int row = -1;
    for (int r = 0; r < tab.table()->rowCount(); ++r) {
      auto *it = tab.table()->item(r, 0);
      if (it && it->text() == QLatin1String("StaleNexus"))
        row = r;
    }
    check(row >= 0, "manifest repair: stale Nexus row surfaces");
    if (row >= 0) {
      check(tab.table()->item(row, 1)->text() == "Manual",
            "manifest repair: stale Nexus label coerced to Manual");
    }
    const auto c = double_click_and_capture(tab, "StaleNexus");
    check(c.fired && c.source_type.empty(),
          "manifest repair: install is manual (no fabricated Nexus)");
  }

  // --- Manifest repair: LoversLab without page_url -> Manual. ---
  {
    const auto zip = dl_dir / "StaleLL.zip";
    write_file(zip, 256);
    TestDownloadsTab tab;
    const std::string stale_ll_json =
        "[{\"id\":\"stale-ll\",\"name\":\"StaleLL\",\"source\":\"LoversLab\","
        "\"file_path\":\"" +
        zip.string() +
        "\","
        "\"state\":2,\"total_size\":0,\"parent_mod_id\":\"\","
        "\"file_id\":0,\"domain\":\"\",\"category\":\"\",\"page_url\":\"\"}]";
    tab.deserialize(stale_ll_json, dl_dir);

    int row = -1;
    for (int r = 0; r < tab.table()->rowCount(); ++r) {
      auto *it = tab.table()->item(r, 0);
      if (it && it->text() == QLatin1String("StaleLL"))
        row = r;
    }
    check(row >= 0, "manifest repair: stale LoversLab row surfaces");
    if (row >= 0) {
      check(tab.table()->item(row, 1)->text() == "Manual",
            "manifest repair: stale LoversLab label coerced to Manual");
    }
  }
}

// MO2 parity for the downloads-tab double-click (Workspace-o03). MO2's
// downloadlistview drives a double-click off the row's state: READY (and
// above) installs, PAUSED resumes, DOWNLOADING does nothing. The install
// half already existed; the resume half was missing (a paused row fell
// through the handler and did nothing at all), so the two signals are
// asserted per state to keep the branches from bleeding into each other:
// a paused row must resume and NOT install, a finished row must install
// and NOT resume, an in-flight row must do neither.
//
// Direct signal capture, no waits: cellDoubleClicked is invoked with
// Qt::DirectConnection so the handler has fully run by the time
// invokeMethod returns, and the signals are emitted synchronously from it.
TEST_CASE("downloads double-click state gating", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_dblclick_gate/config";
  std::filesystem::remove_all("/tmp/gmm_dblclick_gate");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const std::filesystem::path dl_dir = "/tmp/gmm_dblclick_gate/dl";
  std::filesystem::create_directories(dl_dir);

  // Every row gets a real on-disk archive: the install branch is gated on
  // std::filesystem::exists(file_path), so a missing file would make the
  // "finished row installs" assertion pass for the wrong reason.
  const auto paused_zip = dl_dir / "Paused Mod.zip";
  const auto active_zip = dl_dir / "Active Mod.zip";
  const auto done_zip   = dl_dir / "Done Mod.zip";
  const auto failed_zip = dl_dir / "Broken Mod.zip";
  write_file(paused_zip, 256);
  write_file(active_zip, 256);
  write_file(done_zip, 256);
  write_file(failed_zip, 256);

  TestDownloadsTab tab;
  tab.add_download("paused-1", "Paused Mod", "Nexus Mods", paused_zip);
  tab.add_download("active-1", "Active Mod", "Nexus Mods", active_zip);
  tab.add_download("done-1", "Done Mod", "Nexus Mods", done_zip);
  tab.add_download("failed-1", "Broken Mod", "Nexus Mods", failed_zip);
  tab.mark_paused("paused-1");           // Downloading -> Paused
  tab.mark_complete("done-1", true);     // Downloading -> Complete
  tab.mark_complete("failed-1", false);  // Downloading -> Failed
  // active-1 stays Downloading (the state add_download starts in).

  std::vector<std::string> resumed;
  std::vector<std::string> installed;
  QObject::connect(&tab, &ui::DownloadsTab::resume_requested,
                   [&](const std::string &id) { resumed.push_back(id); });
  QObject::connect(&tab, &ui::DownloadsTab::install_requested,
                   [&](const std::string &id, const std::filesystem::path &,
                       const std::string &, const std::string &, int, const std::string &,
                       const std::string &) { installed.push_back(id); });

  auto double_click = [&](const char *name) {
    const int row = row_with_name(tab.table(), name);
    check(row >= 0, "row present before double-click");
    if (row < 0)
      return;
    QMetaObject::invokeMethod(tab.table(), "cellDoubleClicked", Qt::DirectConnection,
                              Q_ARG(int, row), Q_ARG(int, 0));
    app.processEvents();
  };

  double_click("Paused Mod");
  check(resumed.size() == 1 && resumed[0] == "paused-1",
        "double-click on a Paused row emits resume_requested for it");
  check(installed.empty(), "double-click on a Paused row does NOT install");

  double_click("Active Mod");
  check(resumed.size() == 1,
        "double-click on a Downloading row does NOT resume (context menu only)");
  check(installed.empty(), "double-click on a Downloading row does NOT install");

  double_click("Done Mod");
  check(resumed.size() == 1, "double-click on a Complete row does NOT resume");
  check(installed.size() == 1 && installed[0] == "done-1",
        "double-click on a Complete row emits install_requested for it");

  // Failed is a deliberate GMM extra over MO2: a broken download still has
  // its partial archive on disk, so re-offering Install is the recovery
  // path. Locked here so a later MO2-parity sweep does not silently drop it.
  double_click("Broken Mod");
  check(resumed.size() == 1, "double-click on a Failed row does NOT resume");
  check(installed.size() == 2 && installed[1] == "failed-1",
        "double-click on a Failed row still offers install (GMM extra)");
}

// Resume on a row with no known source link must leave the state alone
// (downloads_controller.cpp, resume_requested handler).
//
// A row restored from the download manifest - or any row on a window that
// never started it - has no nxm/modl/url link: those maps are in-memory only
// and deserialize() restores the state, never the link. Marking the row
// Downloading first stranded it forever (nothing gets queued, so the worker
// never emits download_complete/paused), the context menu then offered Pause
// instead of Resume, and has_active_download() kept scan_downloads_dir()
// early-returning for the rest of the session.
//
// Driven through the real MainWindow so the production controller lambda is
// the thing under test; a fresh window has no link entries, which is exactly
// the state a manifest-restored row is in. The Status cell (column 2) is the
// rendered state: Paused is a "Paused" label, Downloading is a QProgressBar
// with format "Starting...".
TEST_CASE("downloads resume without a link keeps the row paused", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path root = "/tmp/gmm_resume_nolink";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "config");
  qputenv("XDG_CONFIG_HOME", (root / "config").c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");
  // Same offscreen hang guard as the MainWindow tests: the nxm handler pops a
  // modal QMessageBox inside the first processEvents.
  Settings::instance().set_nxm_handler_check("dont_ask");

  // The guard's warn is the only observable proof the handler reached the
  // link-miss branch and did not bail on the earlier pipeline_thread_ check.
  // Function-local static on purpose: Logger::add_callback cannot be removed,
  // so a sink captured by reference from a TEST_CASE local would dangle for
  // the rest of the process. This one is constructed before the Logger
  // singleton, so it is destroyed after it and no callback can outlive it.
  static std::vector<std::string> warnings;
  engine::Logger::instance().add_callback(
      [](engine::LogLevel level, const std::string &,
         const std::string &message) {
        if (level == engine::LogLevel::Warn)
          warnings.push_back(message);
      });
  warnings.clear();  // drop the replay buffer handed to late subscribers

  ui::MainWindow w;
  auto *rp = w.findChild<ui::RightPanel *>();
  REQUIRE(rp != nullptr);
  // The downloads tab is a lazy capability and always present in the bar;
  // materializing it is what runs wire_downloads_tab(), i.e. what connects
  // the resume handler this case exercises.
  rp->set_game("testgame");
  auto *tab = rp->ensure_downloads_tab();
  REQUIRE(tab != nullptr);
  auto *table = tab->table();
  REQUIRE(table != nullptr);

  tab->add_download("orphan-1", "Orphan Mod", "Nexus Mods");
  tab->mark_paused("orphan-1");
  const int row = row_with_name(table, "Orphan Mod");
  REQUIRE(row >= 0);
  // Baseline: Paused renders a label, no progress bar.
  REQUIRE(table->cellWidget(row, 2) == nullptr);
  REQUIRE(table->item(row, 2) != nullptr);
  REQUIRE(table->item(row, 2)->text() == QLatin1String("Paused"));

  warnings.clear();
  // The path PR #178 added: cell double-click -> resume_requested(id) ->
  // the controller's handler.
  QMetaObject::invokeMethod(table, "cellDoubleClicked", Qt::DirectConnection,
                            Q_ARG(int, row), Q_ARG(int, 0));
  app.processEvents();

  // State unchanged: still Paused, not a fake Downloading. Guarded on the
  // item so a regression reports the two CHECKs instead of segfaulting on a
  // null deref (mark_downloading deletes the Status item).
  auto *status = table->item(row, 2);
  CHECK(table->cellWidget(row, 2) == nullptr);
  CHECK(status != nullptr);
  if (status)
    CHECK(status->text() == QLatin1String("Paused"));
  // And the handler said why instead of dying silently.
  const auto logged = std::any_of(warnings.begin(), warnings.end(), [](const std::string &m) {
    return m.find("orphan-1") != std::string::npos &&
           m.find("no NXM/modl/URL link") != std::string::npos;
  });
  CHECK(logged);
}

// MO2 column parity for the downloads list, locked against the rendered table.
//
// MO2 (references/modorganizer/src/downloadlist.h:38-50) has 8 columns:
// COL_NAME, COL_STATUS, COL_SIZE, COL_FILETIME, COL_MODNAME, COL_VERSION,
// COL_ID, COL_SOURCEGAME. Of the four this tab was missing it can honestly
// populate two, both appended so the index-keyed header state written by
// SettingsController::save_app_state keeps its meaning for sections 0-3.
TEST_CASE("downloads column set matches MO2", "[ui][mo2-parity]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_downloads_columns/config";
  std::filesystem::remove_all("/tmp/gmm_downloads_columns");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int         argc    = 1;
  char        arg0[]  = "test";
  char       *argv[]  = {arg0, nullptr};
  QApplication app(argc, argv);

  TestDownloadsTab tab;
  auto           *table = tab.table();
  REQUIRE(table != nullptr);

  // The four columns the tab already had keep their indices - a saved
  // header state (SettingsController::save_app_state, keyed by the tab
  // name) stores per-section width/visibility by index, so moving one of
  // them would reinterpret everything a previous build wrote.
  const QStringList expected = {
      QStringLiteral("Name"), QStringLiteral("Source"), QStringLiteral("Status"),
      QStringLiteral("Size"), QStringLiteral("Filetime"), QStringLiteral("Nexus ID"),
  };
  INFO("column count: " << table->columnCount());
  check(table->columnCount() == expected.size(), "tab carries MO2's shippable columns");

  for (int c = 0; c < expected.size(); ++c) {
    auto *header = table->horizontalHeaderItem(c);
    INFO("column " << c);
    REQUIRE(header != nullptr);
    check(header->text() == expected[c], "header label matches the column spec");
    // A header with no tooltip shows nothing on hover; the list is
    // positional, so an empty entry means every later tooltip is off by one.
    check(!header->toolTip().isEmpty(), "every header carries a tooltip");
  }

  // MO2 hides COL_ID on a fresh profile and shows COL_FILETIME
  // (downloadlistview.cpp:147-151). This build shows Name, Status and Size
  // alone, so Source and Filetime join Nexus ID in the default-hidden set -
  // a deliberate departure, and all three are one click away in the header's
  // toggle menu.
  auto *header = table->horizontalHeader();
  check(!header->isSectionHidden(0), "Name is visible by default");
  check(header->isSectionHidden(1), "Source is hidden by default");
  check(!header->isSectionHidden(2), "Status is visible by default");
  check(!header->isSectionHidden(3), "Size is visible by default");
  check(header->isSectionHidden(4), "Filetime is hidden by default");
  check(header->isSectionHidden(5), "Nexus ID is hidden by default");
}

TEST_CASE("downloads new columns carry the data they claim", "[ui][mo2-parity]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg  = "/tmp/gmm_downloads_columns_data/config";
  const std::filesystem::path dl   = "/tmp/gmm_downloads_columns_data/dl";
  std::filesystem::remove_all("/tmp/gmm_downloads_columns_data");
  std::filesystem::create_directories(cfg);
  std::filesystem::create_directories(dl);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int         argc    = 1;
  char        arg0[]  = "test";
  char       *argv[]  = {arg0, nullptr};
  QApplication app(argc, argv);

  const auto nexus_zip = dl / "Nexus Mod.zip";
  const auto local_zip = dl / "Local Mod.zip";
  write_file(nexus_zip, 512);
  write_file(local_zip, 512);

  TestDownloadsTab tab;
  auto           *table = tab.table();

  // A Nexus row: mod id, file id and an archive on disk.
  tab.add_download("1234-5678", "Nexus Mod", "Nexus Mods", nexus_zip,
                   "skyrimspecialedition", 5678, "1234");
  // A row that came from a local archive: no Nexus origin at all.
  tab.add_download("Local Mod", "Local Mod", "Manual", local_zip);

  const int nexus_row = row_with_name(table, "Nexus Mod");
  const int local_row = row_with_name(table, "Local Mod");
  REQUIRE(nexus_row >= 0);
  REQUIRE(local_row >= 0);

  // Nexus ID: the mod id off parent_mod_id, blank for everything else.
  auto *id_item = table->item(nexus_row, 5);
  REQUIRE(id_item != nullptr);
  check(id_item->text() == QLatin1String("1234"), "Nexus ID shows the mod id");
  auto *local_id = table->item(local_row, 5);
  REQUIRE(local_id != nullptr);
  check(local_id->text().isEmpty(), "Nexus ID is blank for a non-Nexus download");

  // Filetime: archive mtime, so a download that has not landed a file yet
  // is blank rather than showing a made-up time.
  auto *ft_item = table->item(nexus_row, 4);
  REQUIRE(ft_item != nullptr);
  // MO2 shows the file's creation time, falling back through the times a
  // filesystem may not record (downloadmanager.cpp:1441-1447), so either
  // stamp is a pass - a freshly written test archive has them identical.
  const QFileInfo nexus_info(QString::fromStdString(nexus_zip.string()));
  const QString want_birth =
      QLocale().toString(nexus_info.birthTime(), QLocale::ShortFormat);
  const QString want_mtime =
      QLocale().toString(nexus_info.lastModified(), QLocale::ShortFormat);
  INFO("filetime rendered: " << ft_item->text().toStdString());
  check(ft_item->text() == want_birth || ft_item->text() == want_mtime,
        "Filetime shows the archive's creation time");
  check(!ft_item->text().isEmpty(), "Filetime is populated for a finished download");

  tab.add_download("pending-1", "Mod #9 - file 1", "Nexus Mods", {}, "skyrimspecialedition",
                   1, "9");
  const int pending_row = row_with_name(table, "Mod #9 - file 1");
  REQUIRE(pending_row >= 0);
  auto *pending_ft = table->item(pending_row, 4);
  REQUIRE(pending_ft != nullptr);
  check(pending_ft->text().isEmpty(), "Filetime is blank while the archive is not on disk");
  auto *pending_id = table->item(pending_row, 5);
  REQUIRE(pending_id != nullptr);
  check(pending_id->text() == QLatin1String("9"),
        "Nexus ID is known before the file lands");

  // set_file_path is the path a Nexus download takes when the archive only
  // becomes known after the fetch; Filetime must follow it.
  const auto late_zip = dl / "Late Mod.zip";
  write_file(late_zip, 512);
  tab.set_file_path("pending-1", late_zip);
  auto *late_ft = table->item(pending_row, 4);
  REQUIRE(late_ft != nullptr);
  const QFileInfo late_info(QString::fromStdString(late_zip.string()));
  check(late_ft->text() == QLocale().toString(late_info.birthTime(), QLocale::ShortFormat) ||
            late_ft->text() == QLocale().toString(late_info.lastModified(), QLocale::ShortFormat),
        "Filetime follows set_file_path");
}

// The header state for this tab is a QHeaderView blob keyed by tab name
// (SettingsController::save_app_state / restore_app_state), which stores
// per-section size and visibility by INDEX. Two things follow from that,
// and both are the reason new columns are appended rather than inserted:
// a saved width must still land on the same header, and a state written by
// a build that predates a column must not decide that column's visibility.
//
// The second one is not free: Qt applies a shorter saved state to the
// leading sections and leaves the rest VISIBLE, so restoring a legacy blob
// silently unhides everything appended after it. SettingsController has to
// re-apply the default-hidden set afterwards, and this drives the same
// sequence the real startup runs.
TEST_CASE("downloads header state survives appending columns", "[ui][mo2-parity]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_downloads_header_state/config";
  std::filesystem::remove_all("/tmp/gmm_downloads_header_state");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int         argc    = 1;
  char        arg0[]  = "test";
  char       *argv[]  = {arg0, nullptr};
  QApplication app(argc, argv);

  // "Saved" state from a build that had only the original four columns:
  // Size widened, Source hidden.
  QByteArray legacy;
  {
    QTableWidget old(0, 4);
    old.horizontalHeader()->resizeSection(3, 321);
    old.horizontalHeader()->setSectionHidden(1, true);
    legacy = old.horizontalHeader()->saveState();
  }

  TestDownloadsTab tab;
  auto           *table  = tab.table();
  auto           *header = table->horizontalHeader();
  check(header->isSectionHidden(5), "Nexus ID starts hidden on a fresh tab");

  header->restoreState(legacy);
  INFO("restored section 3 width: " << header->sectionSize(3));
  check(header->sectionSize(3) == 321, "saved width still lands on Size");
  check(header->isSectionHidden(1), "saved hidden flag still lands on Source");
  // Documented Qt behaviour, asserted so a future Qt bump that changes it
  // is noticed here rather than showing up as a column that reappears for
  // everyone upgrading from a 4-column build.
  check(!header->isSectionHidden(5),
        "restoreState alone unhides a column the saved state predates");

  // ... which is why the re-apply runs after every restore.
  ui::DownloadsTab::apply_default_hidden_columns(table);
  check(header->isSectionHidden(5), "the default-hidden set is re-applied after a restore");
  check(header->sectionSize(3) == 321, "the re-apply leaves saved widths alone");
  check(header->isSectionHidden(1), "the re-apply leaves the user's hidden flag alone");

  // A state written by the current build round-trips over the new columns.
  header->restoreState(header->saveState());
  check(header->sectionSize(3) == 321, "a current-build state round-trips");
  check(header->isSectionHidden(5), "Nexus ID stays hidden across the round-trip");
}

// The spec is on the widget, next to the enum it must stay aligned with.
// A header label list and a tooltip list are both positional, so a column
// added without extending them shifts every later tooltip onto the wrong
// header; that is what these lock down.
TEST_CASE("downloads column spec is positionally complete", "[ui][mo2-parity]") {
  const auto tooltips = ui::DownloadsTab::header_tooltips();
  INFO("tooltip count: " << tooltips.size() << ", column count: " << ui::DownloadsTab::ColumnCount);
  check(static_cast<int>(tooltips.size()) == ui::DownloadsTab::ColumnCount,
        "the tooltip list is exactly as long as the column list");
  for (int c = 0; c < ui::DownloadsTab::ColumnCount; ++c) {
    INFO("column " << c);
    check(!ui::DownloadsTab::column_name(c).isEmpty(), "every column has a name");
    INFO("tooltip: " << tooltips.value(c).toStdString());
    check(!tooltips.value(c).isEmpty(), "every column has a non-empty tooltip");
  }
  check(ui::DownloadsTab::column_name(-1).isEmpty(), "an out-of-range column has no name");
  check(ui::DownloadsTab::column_name(ui::DownloadsTab::ColumnCount).isEmpty(),
        "an out-of-range column has no name");

  // MO2's labels (downloadlist.cpp:76-91) for the columns we carry.
  check(ui::DownloadsTab::column_name(ui::DownloadsTab::Name) == QLatin1String("Name"),
        "Name");
  check(ui::DownloadsTab::column_name(ui::DownloadsTab::Status) == QLatin1String("Status"),
        "Status");
  check(ui::DownloadsTab::column_name(ui::DownloadsTab::Size) == QLatin1String("Size"),
        "Size");
  check(ui::DownloadsTab::column_name(ui::DownloadsTab::Filetime) == QLatin1String("Filetime"),
        "Filetime");
  check(ui::DownloadsTab::column_name(ui::DownloadsTab::NexusId) == QLatin1String("Nexus ID"),
        "Nexus ID");

  // The default-hidden set, in both directions: every column except Name,
  // Status and Size, and nothing else. MO2 keeps Filetime visible, so this is
  // a departure from it rather than a copy.
  const QStringList hidden = ui::DownloadsTab::default_hidden_column_names();
  check(hidden.size() == 3, "three default-hidden columns");
  check(hidden.contains(ui::DownloadsTab::column_name(ui::DownloadsTab::Source)),
        "Source is hidden by default");
  check(hidden.contains(ui::DownloadsTab::column_name(ui::DownloadsTab::Filetime)),
        "Filetime is hidden by default");
  check(hidden.contains(ui::DownloadsTab::column_name(ui::DownloadsTab::NexusId)),
        "Nexus ID is hidden by default");
  check(!hidden.contains(ui::DownloadsTab::column_name(ui::DownloadsTab::Name)),
        "Name is not hidden by default");
  check(!hidden.contains(ui::DownloadsTab::column_name(ui::DownloadsTab::Status)),
        "Status is not hidden by default");
  check(!hidden.contains(ui::DownloadsTab::column_name(ui::DownloadsTab::Size)),
        "Size is not hidden by default");

  // column_names() is what both the table header and the toggle menu read, so
  // its entries must name every column - a blank one would put an empty menu
  // entry where the old code used to fall back to "Column N".
  const QStringList names = ui::DownloadsTab::column_names();
  check(names.size() == ui::DownloadsTab::ColumnCount,
        "column_names() is exactly as long as the column list");
  for (int c = 0; c < ui::DownloadsTab::ColumnCount; ++c) {
    INFO("column " << c);
    check(names.value(c) == ui::DownloadsTab::column_name(c),
          "column_names() keeps column order");
    check(!names.value(c).isEmpty(), "every column has a non-empty label");
  }

  // Name resolution runs the way the ctor runs it: every stored name is
  // matched against column_name(), so a name that no longer exists matches
  // nothing and leaves the table alone.
  QSet<int> resolved;
  for (const QString &name : hidden) {
    for (int c = 0; c < ui::DownloadsTab::ColumnCount; ++c)
      if (ui::DownloadsTab::column_name(c) == name)
        resolved.insert(c);
  }
  check(resolved.size() == 3, "the default-hidden list resolves to exactly three columns");
  check(resolved.contains(ui::DownloadsTab::Source), "and one of them is Source");
  check(resolved.contains(ui::DownloadsTab::Filetime), "and one is Filetime");
  check(resolved.contains(ui::DownloadsTab::NexusId), "and one is Nexus ID");

  // The same resolution over a stored list carrying a retired column name,
  // which is what a future rename would leave behind.
  const QStringList with_retired = {QStringLiteral("Some Retired Column"),
                                    QStringLiteral("Nexus ID")};
  QSet<int>         after_rename;
  for (const QString &name : with_retired) {
    for (int c = 0; c < ui::DownloadsTab::ColumnCount; ++c)
      if (ui::DownloadsTab::column_name(c) == name)
        after_rename.insert(c);
  }
  check(after_rename.size() == 1, "a retired name contributes nothing");
  check(after_rename.contains(ui::DownloadsTab::NexusId), "the live name still resolves");
}

// The header context menu is built from a positional label list, and a short
// one ships unnamed entries: whatever sits past the end renders as "Column N".
// That is how Filetime and Nexus ID ended up unlabelled in the menu. These
// walk the real wiring (RightPanel::build_tab, not a hand-built stand-in) for
// every tab that has a toggle header and assert a label per section, so
// appending a column without a label fails here.
TEST_CASE("every toggle header labels every column", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path root = "/tmp/gmm_toggle_header_labels";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "config");
  qputenv("XDG_CONFIG_HOME", (root / "config").c_str());
  int         test_argc     = 1;
  char        test_argv0[] = "test";
  char       *test_argv[]  = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");
  // Same offscreen hang guard as the MainWindow tests: the nxm handler pops a
  // modal QMessageBox inside the first processEvents.
  Settings::instance().set_nxm_handler_check("dont_ask");

  // A game-less window with no instance: these cases only need the tabs built.
  // Declared BEFORE the window: RightPanel::set_capabilities keeps the raw
  // pointer, so it must outlive MainWindow.
  engine::GameCapabilities caps;
  for (const char *capability : {"plugins", "saves"}) {
    engine::CapabilityInfo info;
    info.game_id      = "togglegamelab";
    info.capability   = capability;
    info.display_name = capability;
    caps.register_capability(info);
  }

  ui::MainWindow w;
  auto *rp = w.findChild<ui::RightPanel *>();
  REQUIRE(rp != nullptr);
  rp->set_capabilities(&caps);
  rp->set_game("togglegamelab");

  // Downloads is instance-owned, so it is in the tab bar for every game.
  auto *downloads = rp->ensure_downloads_tab();
  REQUIRE(downloads != nullptr);
  auto *plugins = rp->ensure_plugins_tab();
  REQUIRE(plugins != nullptr);
  auto *saves = rp->ensure_saves_tab();
  REQUIRE(saves != nullptr);
  auto *data = rp->data_tab();
  REQUIRE(data != nullptr);

  // Each tab that has a toggle header, with the widget whose section count the
  // header's label list has to match.
  const std::vector<std::pair<const char *, QWidget *>> checked = {
      {"downloads", downloads},
      {"plugins", plugins},
      {"saves", saves},
      {"data", data},
  };
  for (const auto &[name, widget] : checked) {
    INFO("tab: " << name);
    QHeaderView *raw = nullptr;
    if (auto *table = widget->findChild<QTableWidget *>())
      raw = table->horizontalHeader();
    else if (auto *tree = widget->findChild<QTreeWidget *>())
      raw = tree->header();
    REQUIRE(raw != nullptr);
    auto *header = qobject_cast<ui::ColumnToggleHeaderView *>(raw);
  }

  // The downloads menu is the one that shipped broken: pin the two labels it
  // was missing, and that they match the tab's own column list.
  auto *dl_table   = downloads->table();
  auto *dl_header  = qobject_cast<ui::ColumnToggleHeaderView *>(dl_table->horizontalHeader());
  REQUIRE(dl_header != nullptr);
  CHECK(dl_header->column_labels() == ui::DownloadsTab::column_names());
  CHECK(dl_header->column_labels().value(ui::DownloadsTab::Filetime) ==
        QLatin1String("Filetime"));
  CHECK(dl_header->column_labels().value(ui::DownloadsTab::NexusId) ==
        QLatin1String("Nexus ID"));

  // The mod list's header is the other ColumnToggleHeaderView; Fold carries no
  // name of its own, so its entry is empty by design.
  auto *mod_view = w.findChild<ui::ModView *>();
  REQUIRE(mod_view != nullptr);
  auto *mod_header =
      qobject_cast<ui::ColumnToggleHeaderView *>(mod_view->header());
  REQUIRE(mod_header != nullptr);
  INFO("mod list label count: " << mod_header->column_labels().size()
                                << ", section count: " << mod_header->count());
  CHECK(static_cast<int>(mod_header->column_labels().size()) == mod_header->count());
  CHECK(mod_header->column_labels().value(ui::ModList::ColumnCount - 1) ==
        QLatin1String("Priority"));
}

// apply_default_hidden_columns() is what makes a changed default reach a user
// who already has a saved header state, so it has to un-hide as well as hide:
// a saved blob from the previous default shows Source and Filetime, and hiding
// alone would leave them on screen.
TEST_CASE("default hidden columns apply in both directions",
          "[ui][mo2-parity]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_downloads_default_both_ways/config";
  std::filesystem::remove_all("/tmp/gmm_downloads_default_both_ways");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int         test_argc     = 1;
  char        test_argv0[] = "test";
  char       *test_argv[]  = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);

  const QStringList hidden = ui::DownloadsTab::default_hidden_column_names();

  // Every column showing, as a saved state under the old default would leave
  // them (all but Nexus ID).
  QTableWidget all_visible(0, ui::DownloadsTab::ColumnCount);
  ui::DownloadsTab::apply_default_hidden_columns(&all_visible);
  for (int c = 0; c < ui::DownloadsTab::ColumnCount; ++c) {
    INFO("column " << c);
    check(all_visible.horizontalHeader()->isSectionHidden(c) ==
              hidden.contains(ui::DownloadsTab::column_name(c)),
          "visibility follows the default-hidden list");
  }
  check(!all_visible.horizontalHeader()->isSectionHidden(ui::DownloadsTab::Name),
        "Name is shown");
  check(!all_visible.horizontalHeader()->isSectionHidden(ui::DownloadsTab::Status),
        "Status is shown");
  check(!all_visible.horizontalHeader()->isSectionHidden(ui::DownloadsTab::Size),
        "Size is shown");
  check(all_visible.horizontalHeader()->isSectionHidden(ui::DownloadsTab::Source),
        "a column the old default left showing is hidden");
  check(all_visible.horizontalHeader()->isSectionHidden(ui::DownloadsTab::Filetime),
        "a column the old default left showing is hidden");

  // Everything hidden, as a user who hid every column by hand would leave it:
  // the three that are not in the list have to come back.
  QTableWidget all_hidden(0, ui::DownloadsTab::ColumnCount);
  for (int c = 0; c < ui::DownloadsTab::ColumnCount; ++c)
    all_hidden.horizontalHeader()->setSectionHidden(c, true);
  ui::DownloadsTab::apply_default_hidden_columns(&all_hidden);
  check(!all_hidden.horizontalHeader()->isSectionHidden(ui::DownloadsTab::Name),
        "Name is shown again");
  check(!all_hidden.horizontalHeader()->isSectionHidden(ui::DownloadsTab::Status),
        "Status is shown again");
  check(!all_hidden.horizontalHeader()->isSectionHidden(ui::DownloadsTab::Size),
        "Size is shown again");
  check(all_hidden.horizontalHeader()->isSectionHidden(ui::DownloadsTab::NexusId),
        "Nexus ID stays hidden");

  // Idempotent, and a null table is a no-op.
  ui::DownloadsTab::apply_default_hidden_columns(&all_visible);
  for (int c = 0; c < ui::DownloadsTab::ColumnCount; ++c)
    check(all_visible.horizontalHeader()->isSectionHidden(c) ==
              hidden.contains(ui::DownloadsTab::column_name(c)),
          "a second application changes nothing");
  ui::DownloadsTab::apply_default_hidden_columns(nullptr);
  check(true, "a null table is ignored");
}

namespace {

// An instance whose last selected right-panel tab is Downloads, so the tab is
// built before set_game_info() restores the app state - the only way a saved
// header blob reaches a table at all (the restore walks built tabs, and the
// other tabs are still placeholders at that point).
struct AppStateHarness {
  engine::GameKnowledge knowledge;
  engine::GameCapabilities caps;

  void open(const std::filesystem::path &root) {
    auto              inst           = engine::Instance::installed("TestGame", root);
    inst.info().game_id = "statedatalab";
    inst.info().last_tab = "downloads";
    REQUIRE(inst.create_directories());
    REQUIRE(inst.write_toml());
    knowledge.set("statedatalab", "mods_subpath", "Mods");
    instance_root = inst.info().root;
  }

  // Mirrors what the MainWindow tests do: the capabilities pointer is kept by
  // RightPanel, so it has to outlive the window it is handed to.
  void open_window(ui::MainWindow &w) {
    w.set_game_knowledge(&knowledge);
    w.set_game_info("statedatalab", "State Data Lab", "Default", {}, instance_root);
  }

  std::filesystem::path instance_root;
};

ui::DownloadsTab *downloads_tab_of(ui::MainWindow &w) {
  auto *rp = w.findChild<ui::RightPanel *>();
  REQUIRE(rp != nullptr);
  auto *tab = rp->ensure_downloads_tab();
  REQUIRE(tab != nullptr);
  return tab;
}

ui::ColumnToggleHeaderView *downloads_header_of(ui::MainWindow &w) {
  auto *header = qobject_cast<ui::ColumnToggleHeaderView *>(
      downloads_tab_of(w)->table()->horizontalHeader());
  REQUIRE(header != nullptr);
  return header;
}

}  // namespace

// A saved header state records what the columns looked like under whatever
// default was in force when it was written, so restoring it verbatim freezes
// the old visible set in place for everyone who has already run the app. This
// drives the real startup path (set_game_info() restores the app state) with a
// blob written the way the previous default would have left it.
TEST_CASE("a saved downloads header state does not freeze the old visible set",
          "[ui][mo2-parity]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path root = "/tmp/gmm_downloads_saved_default";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "config");
  qputenv("XDG_CONFIG_HOME", (root / "config").c_str());
  int         test_argc     = 1;
  char        test_argv0[] = "test";
  char       *test_argv[]  = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");
  Settings::instance().set_nxm_handler_check("dont_ask");

  AppStateHarness harness;
  harness.open(root);

  {
    ui::MainWindow w;
    harness.open_window(w);
    auto *header = downloads_header_of(w);
    // A fresh tab already ships the new default. The tab's constructor sets
    // this before RightPanel swaps in the toggle header, so this also covers
    // that swap carrying the visibility across.
    for (int c = 0; c < ui::DownloadsTab::ColumnCount; ++c) {
      INFO("column " << c << " on a fresh tab");
      check(header->isSectionHidden(c) ==
                ui::DownloadsTab::default_hidden_column_names()
                    .contains(ui::DownloadsTab::column_name(c)),
            "a fresh tab follows the default-hidden list");
    }

    // ... and the previous default left Source and Filetime showing, with a
    // widened Size. That is the blob an existing user carries.
    header->setSectionHidden(ui::DownloadsTab::Source, false);
    header->setSectionHidden(ui::DownloadsTab::Filetime, false);
    header->resizeSection(ui::DownloadsTab::Size, 321);

    auto *settings = w.findChild<ui::SettingsController *>();
    REQUIRE(settings != nullptr);
    settings->save_app_state();
  }

  {
    ui::MainWindow w;
    harness.open_window(w);
    auto *header = downloads_header_of(w);
    INFO("restored Size width: " << header->sectionSize(ui::DownloadsTab::Size));
    check(header->sectionSize(ui::DownloadsTab::Size) == 321,
          "the saved width survives the restore");
    check(!header->isSectionHidden(ui::DownloadsTab::Name), "Name stays visible");
    check(!header->isSectionHidden(ui::DownloadsTab::Status), "Status stays visible");
    check(!header->isSectionHidden(ui::DownloadsTab::Size), "Size stays visible");
    check(header->isSectionHidden(ui::DownloadsTab::Source),
          "the new default reaches a saved state that showed Source");
    check(header->isSectionHidden(ui::DownloadsTab::Filetime),
          "the new default reaches a saved state that showed Filetime");
    check(header->isSectionHidden(ui::DownloadsTab::NexusId), "Nexus ID stays hidden");
  }
}

// The other half: a blob cannot tell a column the user revealed from the menu
// from one that was simply never hidden, so the header records which of the
// two a saved state is, and a saved state that is a choice is left alone.
TEST_CASE("a downloads column shown from the menu survives a restart",
          "[ui][mo2-parity]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path root = "/tmp/gmm_downloads_saved_choice";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "config");
  qputenv("XDG_CONFIG_HOME", (root / "config").c_str());
  int         test_argc     = 1;
  char        test_argv0[] = "test";
  char       *test_argv[]  = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");
  Settings::instance().set_nxm_handler_check("dont_ask");

  AppStateHarness harness;
  harness.open(root);

  {
    ui::MainWindow w;
    harness.open_window(w);
    auto *header = downloads_header_of(w);
    // The menu's toggle handler does exactly this pair of calls.
    header->setSectionHidden(ui::DownloadsTab::Filetime, false);
    header->note_user_visibility_choice();
    check(!header->isSectionHidden(ui::DownloadsTab::Filetime), "Filetime is showing");

    auto *settings = w.findChild<ui::SettingsController *>();
    REQUIRE(settings != nullptr);
    settings->save_app_state();
  }

  {
    ui::MainWindow w;
    harness.open_window(w);
    auto *header = downloads_header_of(w);
    check(!header->isSectionHidden(ui::DownloadsTab::Filetime),
          "the revealed column is still showing after a restart");
    // The rest of the tab still answers to the default rather than to what the
    // previous default happened to leave: one flag covers the whole tab, so a
    // header the user has used is restored exactly as saved.
    check(header->isSectionHidden(ui::DownloadsTab::NexusId), "Nexus ID stays hidden");
    // Re-seeded onto the header, which is what keeps the next save from
    // dropping the choice on the floor.
    check(header->has_user_visibility_choice(),
          "the choice is recorded on the restored header");
  }
}
