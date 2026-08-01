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
//     archive path (source_type "", i.e. a local archive).
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, no network.
#include "ui/panels/tab_panels.h"

#include <QApplication>
#include <QTableWidget>
#include <QTableWidgetItem>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

static int failures = 0;
static int passes = 0;
static void check(bool cond, const char* what) {
    std::printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
    if (cond)
        ++passes;
    else
        ++failures;
}

// Write a fake archive (any bytes will do - the tab only cares about the name
// and size).
static void write_file(const std::filesystem::path& path, size_t size) {
    std::ofstream out(path, std::ios::binary);
    out.write("x", 1);
    for (size_t i = 1; i < size; ++i) out.put(static_cast<char>('a' + (i % 26)));
}

// Row index whose Name column equals `name`, or -1.
static int row_with_name(QTableWidget* table, const char* name) {
    for (int r = 0; r < table->rowCount(); ++r) {
        auto* it = table->item(r, 0);
        if (it && it->text() == QLatin1String(name)) return r;
    }
    return -1;
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const std::filesystem::path cfg = "/tmp/gmm_downloads_tab/config";
    std::filesystem::remove_all("/tmp/gmm_downloads_tab");
    std::filesystem::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

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

    ui::DownloadsTab tab;

    // Pre-seed a tracked entry whose on-disk archive is tracked_zip (the key
    // is "<mod_id>-<file_id>", not the filename). Mark it complete first so
    // the scan is not blocked by the active-download guard.
    tab.add_download("32444-1234", "Tracked File", "Nexus Mods", tracked_zip,
                     "skyrimspecialedition", 1234, "32444");
    tab.mark_complete("32444-1234", true);

    // Trigger the scan via set_downloads_dir (what MainWindow does on startup).
    tab.set_downloads_dir(dl_dir);

    auto* table = tab.table();
    check(table->rowCount() == 2,
          "scan adds only the untracked archive (dedupes the tracked file)");

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
            int, const std::string& name) {
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

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
