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
#include <QHeaderView>
#include <QMenu>
#include <QMimeData>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QUrl>

#include "ui/settings/settings.h"
#include "ui/theme/icon_manager.h"

#include <QByteArray>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char* what) {
    INFO(what);
    REQUIRE(cond);
}
}

// Write a fake archive (any bytes will do - the tab only cares about the name
// and size).
static void write_file(const std::filesystem::path& path, size_t size) {
    std::ofstream out(path, std::ios::binary);
    out.write("x", 1);
    for (size_t i = 1; i < size; ++i) out.put(static_cast<char>('a' + (i % 26)));
}

// 1x1 transparent PNG - a real image so the vendor icon actually loads.
static const char* kPngB64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";
static void write_png(const std::filesystem::path& p) {
    const QByteArray bytes = QByteArray::fromBase64(kPngB64);
    std::ofstream out(p, std::ios::binary);
    out.write(bytes.constData(), bytes.size());
}

// Row index whose Name column equals `name`, or -1.
static int row_with_name(QTableWidget* table, const char* name) {
    for (int r = 0; r < table->rowCount(); ++r) {
        auto* it = table->item(r, 0);
        if (it && it->text() == QLatin1String(name)) return r;
    }
    return -1;
}

// Expose the protected drag/drop handlers so the drop flow can be driven
// directly (see the delivery caveat in the file header).
struct TestDownloadsTab : ui::DownloadsTab {
    using ui::DownloadsTab::dragEnterEvent;
    using ui::DownloadsTab::dragMoveEvent;
    using ui::DownloadsTab::dropEvent;
    using ui::DownloadsTab::add_context_menu_actions;
};

TEST_CASE("downloads tab", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const std::filesystem::path cfg = "/tmp/gmm_downloads_tab/config";
    std::filesystem::remove_all("/tmp/gmm_downloads_tab");
    std::filesystem::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
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
    auto* nexus_src = tab.table()->item(0, 1);
    check(nexus_src && !nexus_src->icon().isNull(),
          "Nexus Mods row carries the branded vendor icon");

    // Trigger the scan via set_downloads_dir (what MainWindow does on startup).
    tab.set_downloads_dir(dl_dir);

    auto* table = tab.table();
    check(table->rowCount() == 2,
          "scan adds only the untracked archive (dedupes the tracked file)");

    // The scanned Manual row must NOT carry a vendor icon.
    auto* manual_src = table->item(1, 1);
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
        check(table->rowHeight(r) == standard_h,
              "rows re-heigh on compact toggle");
    }
    Settings::instance().set_compact_downloads(true);
    tab.apply_compact_style();
    check(table->rowHeight(0) == compact_h,
          "rows shrink back after toggling compact on");

    const int manual_row = row_with_name(table, "My Mod");
    check(manual_row >= 0, "untracked archive listed with its stem as name");
    if (manual_row >= 0) {
        check(table->item(manual_row, 1)->text() == "Manual",
              "untracked row has Manual source");
        check(table->item(manual_row, 2)->text() == "Install",
              "untracked row shows Install (Complete) status");
        check(table->item(manual_row, 3)->text() == "2.0 KB",
              "untracked row shows the archive size");
        auto* status = table->item(manual_row, 2);
        check(status->foreground().color().name() == "#4caf50" &&
                  status->background().style() == Qt::NoBrush,
              "Complete status: normal background, green Install text");
    }
    check(row_with_name(table, "Tracked File-32444-11-1234.zip") < 0,
          "archive backing a tracked entry is not duplicated");
    check(row_with_name(table, "notes.txt") < 0,
          "non-archive files are ignored");
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
    check(row_with_name(table, "More Mod") < 0,
          "scan skipped while an entry is Paused");
    tab.mark_complete("dl-2", false);
    tab.set_downloads_dir(dl_dir);
    check(row_with_name(table, "More Mod") >= 0,
          "scan runs again after the paused entry resolves");
    {
        const int failed_row = row_with_name(table, "Paused dl");
        check(failed_row >= 0, "failed row present");
        if (failed_row >= 0) {
            auto* status = table->item(failed_row, 2);
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
            "\"file_path\":\"" + manual_zip.string() + "\",\"state\":5,\"total_size\":0}]";
        tab.deserialize(removed_json, dl_dir);
        const int removed_row = row_with_name(table, "RemovedMod");
        check(removed_row >= 0, "Removed-state entry restored from manifest");
        if (removed_row >= 0) {
            auto* status = table->item(removed_row, 2);
            check(status && status->text() == "Removed" &&
                      status->foreground().color().name() == "#b8860b" &&
                      status->background().style() == Qt::NoBrush,
                  "Removed status renders dark-yellow text on normal background");
        }
    }

    // Double-clicking the untracked row emits install_requested with the real
    // archive path and an empty source type (local archive install).
    bool got_install = false;
    std::string got_source_type = "unset";
    std::filesystem::path got_path;
    QObject::connect(&tab, &ui::DownloadsTab::install_requested,
        [&](const std::string&, const std::filesystem::path& fp,
            const std::string& source_type, const std::string&,
            int, const std::string& name, const std::string&) {
            got_install = true;
            got_path = fp;
            got_source_type = source_type;
            (void)name;
        });
    const int my_row = row_with_name(table, "My Mod");
    check(my_row >= 0, "untracked row present before double-click");
    if (my_row >= 0) {
        QMetaObject::invokeMethod(table, "cellDoubleClicked",
                                  Qt::DirectConnection,
                                  Q_ARG(int, my_row), Q_ARG(int, 0));
        app.processEvents();
        check(got_install && got_path == manual_zip &&
                  got_source_type.empty(),
              "double-click on untracked row emits install_requested (local)");
    }

    // --- External archive drops (MO2 downloads-tab parity) ---
    const std::filesystem::path src_dir = "/tmp/gmm_downloads_tab/src";
    std::filesystem::create_directories(src_dir);

    // Build a drop carrying a single local file URL. move=true proposes
    // MoveAction (Shift drag), move=false proposes CopyAction (Ctrl drag) -
    // matching how Qt derives the proposed action from the modifiers.
    auto send_drop = [&](TestDownloadsTab* target,
                         const std::filesystem::path& file, bool move,
                         bool* accepted = nullptr,
                         Qt::DropAction* drop_action = nullptr) {
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(QString::fromStdString(file.string()))});
        QDropEvent event(QPointF(5, 5),
                         Qt::MoveAction | Qt::CopyAction,
                         &mime, Qt::LeftButton,
                         move ? Qt::ShiftModifier : Qt::ControlModifier);
        target->dropEvent(&event);
        if (accepted) *accepted = event.isAccepted();
        if (drop_action) *drop_action = event.dropAction();
    };

    // Drag gate: only local archive files are accepted (and then with the
    // proposed action, as Qt dictates for the hand-off).
    {
        QMimeData gate_mime;
        gate_mime.setUrls({QUrl::fromLocalFile(QString::fromStdString(
            (src_dir / "Gate Mod.zip").string()))});
        QDragEnterEvent gate(QPoint(5, 5), Qt::MoveAction | Qt::CopyAction,
                             &gate_mime, Qt::LeftButton, Qt::ShiftModifier);
        tab.dragEnterEvent(&gate);
        check(gate.isAccepted(),
              "dragEnterEvent accepts a local archive drop");
        QMimeData bad_mime;
        bad_mime.setUrls({QUrl::fromLocalFile(QString::fromStdString(
            (src_dir / "Readme.txt").string()))});
        QDragEnterEvent bad(QPoint(5, 5), Qt::MoveAction | Qt::CopyAction,
                            &bad_mime, Qt::LeftButton, Qt::ShiftModifier);
        tab.dragEnterEvent(&bad);
        check(!bad.isAccepted(),
              "dragEnterEvent ignores a non-archive drop");
        QMimeData http_mime;
        http_mime.setUrls({QUrl(QStringLiteral("https://example.com/x.zip"))});
        QDragEnterEvent http(QPoint(5, 5), Qt::MoveAction | Qt::CopyAction,
                             &http_mime, Qt::LeftButton, Qt::ShiftModifier);
        tab.dragEnterEvent(&http);
        check(!http.isAccepted(),
              "dragEnterEvent ignores remote URLs");
    }

    // A LoversLab download link dropped from a browser routes through the same
    // "Add from URL…" flow (loverslab_url_entered); a non-LoversLab remote URL
    // is still rejected and archive drops keep the file-import path.
    {
        std::string dropped_url;
        QObject::connect(&tab, &ui::DownloadsTab::loverslab_url_entered,
                         [&dropped_url](const std::string& url) {
                             dropped_url = url;
                         });

        QMimeData ll_mime;
        ll_mime.setUrls({QUrl(QStringLiteral(
            "https://www.loverslab.com/files/file/10093-slug/?do=download&r=7"))});
        QDragEnterEvent ll_enter(QPoint(5, 5), Qt::MoveAction | Qt::CopyAction,
                                 &ll_mime, Qt::LeftButton, Qt::ShiftModifier);
        tab.dragEnterEvent(&ll_enter);
        check(ll_enter.isAccepted(),
              "dragEnterEvent accepts a LoversLab URL drop");

        QDropEvent ll_drop(QPointF(5, 5), Qt::MoveAction | Qt::CopyAction,
                           &ll_mime, Qt::LeftButton, Qt::ShiftModifier);
        tab.dropEvent(&ll_drop);
        check(ll_drop.isAccepted(), "LoversLab URL drop is accepted");
        check(dropped_url ==
                  "https://www.loverslab.com/files/file/10093-slug/?do=download&r=7",
              "dropped LoversLab URL emitted via loverslab_url_entered");

        // A LoversLab page link dropped as bare text is also routed.
        QMimeData ll_text;
        ll_text.setText(QStringLiteral("https://www.loverslab.com/files/file/200/"));
        QDropEvent text_drop(QPointF(5, 5), Qt::MoveAction | Qt::CopyAction,
                             &ll_text, Qt::LeftButton, Qt::ShiftModifier);
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
    bool moved_accepted = false;
    Qt::DropAction moved_action = Qt::IgnoreAction;
    send_drop(&tab, moved_src, true, &moved_accepted, &moved_action);
    check(moved_accepted, "move drop is accepted");
    check(moved_action == Qt::TargetMoveAction,
          "move drop takes the source (TargetMoveAction, MO2 parity)");
    check(!std::filesystem::exists(moved_src),
          "move drop removes the source archive");
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
    bool copied_accepted = false;
    Qt::DropAction copied_action = Qt::IgnoreAction;
    send_drop(&tab, copied_src, false, &copied_accepted, &copied_action);
    check(copied_accepted, "copy drop is accepted");
    check(copied_action == Qt::CopyAction,
          "copy drop leaves the source in place (CopyAction)");
    check(std::filesystem::exists(copied_src),
          "copy drop keeps the source archive");
    check(std::filesystem::exists(dl_dir / "Copied Mod.7z"),
          "copy drop lands a copy in the downloads dir");
    check(row_with_name(table, "Copied Mod") >= 0,
          "copy drop surfaces a row too");

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
    const auto clash_src = src_dir / "Clash Mod.zip";
    const auto clash_dest = dl_dir / "Clash Mod.zip";
    write_file(clash_src, 512);
    write_file(clash_dest, 64);
    tab.set_conflict_resolver([](const std::filesystem::path&,
                                 const std::filesystem::path&) {
        return ui::DropConflictAction::Rename;
    });
    send_drop(&tab, clash_src, true);
    check(std::filesystem::exists(dl_dir / "1_Clash Mod.zip"),
          "conflict Rename lands the archive as 1_<name>");
    check(!std::filesystem::exists(clash_src),
          "conflict Rename still moves the source");
    check(row_with_name(table, "1_Clash Mod") >= 0,
          "conflict Rename row shown under the numbered name");

    // Conflict -> Ignore: nothing is moved, no new entry.
    const auto ignore_src = src_dir / "Ignore Mod.zip";
    const auto ignore_dest = dl_dir / "Ignore Mod.zip";
    write_file(ignore_src, 512);
    write_file(ignore_dest, 64);
    tab.set_conflict_resolver([](const std::filesystem::path&,
                                 const std::filesystem::path&) {
        return ui::DropConflictAction::Ignore;
    });
    send_drop(&tab, ignore_src, true);
    check(std::filesystem::exists(ignore_src),
          "conflict Ignore leaves the source in place");
    check(row_with_name(table, "Ignore Mod") < 0,
          "conflict Ignore adds no row");

    // Conflict -> Overwrite: the existing archive is replaced by the drop.
    const auto over_src = src_dir / "Overwrite Mod.zip";
    const auto over_dest = dl_dir / "Overwrite Mod.zip";
    {
        std::ofstream out(over_src, std::ios::binary);
        out << "new content";
    }
    {
        std::ofstream out(over_dest, std::ios::binary);
        out << "old content";
    }
    tab.set_conflict_resolver([](const std::filesystem::path&,
                                 const std::filesystem::path&) {
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
    const auto re_src = src_dir / "Replaced Mod.zip";
    const auto re_dest = dl_dir / "Replaced Mod.zip";
    write_file(re_dest, 64);
    tab.set_downloads_dir(dl_dir);
    const int re_seed_row = row_with_name(table, "Replaced Mod");
    check(re_seed_row >= 0,
          "existing archive is tracked before the overwrite drop");
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
    const auto re_copy_src = src_dir / "Replaced Copy.zip";
    const auto re_copy_dest = dl_dir / "Replaced Copy.zip";
    write_file(re_copy_dest, 64);
    tab.set_downloads_dir(dl_dir);
    const int rc_seed_row = row_with_name(table, "Replaced Copy");
    check(rc_seed_row >= 0,
          "existing archive is tracked before the copy overwrite");
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
    auto wait_until = [&app](int timeout_ms, const std::function<bool()>& cond) {
        for (int waited = 0; waited < timeout_ms; waited += 10) {
            app.processEvents();
            if (cond()) return true;
            QThread::msleep(10);
        }
        return false;
    };

    TestDownloadsTab watch_tab;
    watch_tab.set_downloads_dir(dl_dir);

    // 1) A new archive landing in the dir appears without a manual scan.
    const auto watched_add = dl_dir / "Watched Add.zip";
    write_file(watched_add, 2048);
    check(wait_until(1500, [&]() {
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
    check(wait_until(1500, [&]() {
              return row_with_name(watch_tab.table(), "Watched Overwrite") >= 0;
          }),
          "watchdog surfaces the initial tracked archive");
    write_file(watched_over, 512);
    check(wait_until(1500, [&]() {
              const int r = row_with_name(watch_tab.table(), "Watched Overwrite");
              return r >= 0 && watch_tab.table()->item(r, 3)->text() == "512 B";
          }),
          "watchdog refreshes the size of an overwritten tracked archive");

    // 3) Deleting an archive removes its row on its own.
    std::filesystem::remove(watched_over);
    check(wait_until(1500, [&]() {
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
    check(wait_until(1500, [&]() {
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
    check(wait_until(1500, [&]() {
              return row_with_name(empty_tab.table(), "Fresh Mod") >= 0;
          }),
          "empty tab: watchdog surfaces a file copied into the dir");

    // 2) Drop onto the empty tab: the archive lands in the dir AND a row
    // appears (the reported flow is drag into a 0-row tab).
    const auto empty_drop_src = src_dir / "Empty Tab Drop.zip";
    write_file(empty_drop_src, 512);
    bool empty_drop_accepted = false;
    send_drop(&empty_tab, empty_drop_src, true,
              &empty_drop_accepted);
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
    check(cycle_tab.table()->rowCount() == 0,
          "add/remove/add: cycle tab starts empty");

    const auto cycle_a = cycle_dir / "Cycle A.zip";
    write_file(cycle_a, 128);
    check(wait_until(1500, [&]() {
              return row_with_name(cycle_tab.table(), "Cycle A") >= 0;
          }),
          "add/remove/add: first archive surfaces (add works)");

    std::filesystem::remove(cycle_a);
    check(wait_until(1500, [&]() {
              return row_with_name(cycle_tab.table(), "Cycle A") < 0;
          }),
          "add/remove/add: external delete removes the row");

    const auto cycle_b = cycle_dir / "Cycle B.zip";
    write_file(cycle_b, 256);
    check(wait_until(1500, [&]() {
              return row_with_name(cycle_tab.table(), "Cycle B") >= 0;
          }),
          "add/remove/add: a new archive still surfaces after a removal");

    const auto cycle_c = cycle_dir / "Cycle C.zip";
    write_file(cycle_c, 512);
    check(wait_until(1500, [&]() {
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

    auto find_action = [](QMenu& menu, const char* text) -> QAction* {
        const QString needle = QString::fromLatin1(text);
        for (auto* act : menu.actions())
            if (act->text() == needle) return act;
        return nullptr;
    };

    // Complete LoversLab entry with a real archive on disk.
    const std::string ll_page =
        "https://www.loverslab.com/files/file/4242-skooma-whore-se/";
    const auto ll_zip = dl_dir / "Skooma Whore SE v1.01.zip";
    write_file(ll_zip, 512);
    ctx_tab.add_download("4242", "Skooma Whore SE v1.01", "LoversLab",
                         ll_zip, {}, 0, {}, ll_page);
    ctx_tab.mark_complete("4242", true);
    {
        std::string got_st = "unset", got_sid = "unset", got_page = "unset";
        bool triggered = false;
        QObject::connect(&ctx_tab, &ui::DownloadsTab::install_requested,
            [&](const std::string&, const std::filesystem::path&,
                const std::string& st, const std::string& sid,
                int, const std::string&, const std::string& page) {
                triggered = true; got_st = st; got_sid = sid; got_page = page;
            });
        QMenu menu;
        ctx_tab.add_context_menu_actions(menu, "4242");
        auto* act = find_action(menu, "Open on LoversLab");
        check(act && act->isEnabled() &&
                  act->data().toString().toStdString() == ll_page,
              "context menu: LoversLab row offers 'Open on LoversLab' with the page URL");
        check(!find_action(menu, "Open on Nexus"),
              "context menu: LoversLab row has no 'Open on Nexus' action");
        auto* install = find_action(menu, "Install");
        if (install) install->trigger();
        check(triggered && got_st == "loverslab" && got_sid == "4242" &&
                  got_page == ll_page,
              "context menu: LoversLab Install carries loverslab provenance (id + page URL)");
    }

    // Nexus entry: URL built from domain + parent mod id.
    ctx_tab.add_download("32444-1234", "Tracked File", "Nexus Mods",
                         tracked_zip, "skyrimspecialedition", 1234, "32444");
    {
        QMenu menu;
        ctx_tab.add_context_menu_actions(menu, "32444-1234");
        auto* act = find_action(menu, "Open on Nexus");
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
        f_tab.add_download("t2", "Skooma Whore SE", "LoversLab", ll_zip, {}, 0,
                           {}, ll_page);
        f_tab.mark_complete("t2", true);
        auto* f_tab_tbl = f_tab.table();
        auto row_of = [&](const char* name) {
            for (int r = 0; r < f_tab_tbl->rowCount(); ++r) {
                auto* item = f_tab_tbl->item(r, 0);
                if (item && item->text() == QLatin1String(name)) return r;
            }
            return -1;
        };
        // The tab's only QCheckBox is the "Hide installed" toggle; checking it
        // fires the same toggled handler the UI uses.
        auto* hide_cb = f_tab.findChild<QCheckBox*>();
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
        if (hide_cb) hide_cb->setChecked(true);
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
