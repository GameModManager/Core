#pragma once

#include "engine/game/saves/save_game.h"
#include "engine/game/saves/save_missing_assets.h"
#include "ui/main_window/saves_scan_worker.h"

#include <QPointer>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

class QTableWidget;
class QTableWidgetItem;
class QEvent;
class QShowEvent;

namespace ui {

class SavesTab : public QWidget {
    Q_OBJECT
public:
    explicit SavesTab(QWidget* parent = nullptr);
    ~SavesTab() override;

    [[nodiscard]] QTableWidget* table() const { return table_; }

    // Replace the save list contents (a SavesScanThread result, delivered via
    // its finished signal). Missing-column text and hover info derive from the
    // per-save missing-assets data. Production code now uses per-entry
    // streaming (on_entry_ready); this batch path is kept for tests that
    // construct a SavesScanResult directly (tests/ui/saves_tab_test.cpp).
    void set_saves(SavesScanResult result);

    // The saves directory (used by MainWindow when it builds a scan request).
    // Scans are NOT watched and NOT eager: the first scan runs lazily when
    // the tab is first shown (Workspace-ugm3), and MainWindow re-runs one
    // after a delete. No background re-scans.
    void set_saves_dir(const std::filesystem::path& dir);
    [[nodiscard]] std::filesystem::path saves_dir() const { return saves_dir_; }

    // Game/instance switch: drop the current list.
    void clear_saves();

    // Run one scan on the background thread. The request carries a snapshot of
    // the current load order + dirs, so results reflect the state at the moment
    // the refresh was asked for. MainWindow builds it in answer to a delete
    // or to the lazy first-show trigger (Workspace-ugm3).
    void request_scan(SavesScanRequest request);

    // Lazy-scan entry point (Workspace-ugm3): emits scan_requested exactly
    // once per saves dir; later calls are no-ops until set_saves_dir or
    // clear_saves resets the latch.
    void ensure_scanned();

    // Row's save at `row`, or nullptr when out of range.
    [[nodiscard]] const engine::SaveGame* save_at(int row) const;
    [[nodiscard]] const std::vector<engine::SaveMissingAsset>* missing_at(int row) const;

    // Underlying scan worker (test-only: lets QSignalSpy observe
    // entryReady / finished). Production callers don't need this.
    [[nodiscard]] SavesScanThread* scan_thread() const { return scan_thread_; }

    // Columns (RightPanel sets the toggle header labels).
    static constexpr int kColumnName = 0;
    static constexpr int kColumnFile = 1;
    static constexpr int kColumnMissing = 2;

    // Hard cap on retained saves (Workspace-x5zg). Scan entries are light
    // (Workspace-de5v strips screenshots at scan time), but hover-cached
    // screenshots (SE: 320x192x4 = 240KB) still accumulate, so an uncapped
    // list grows without bound for save-hoarders. Entries stay sorted
    // newest-first; inserts past the cap drop the oldest entry.
    static constexpr std::size_t kMaxSavesRetain = 500;

signals:
    // Delete the named save files (and their .skse co-saves) - MainWindow
    // routes through engine::remove_path (trash), then re-scans.
    void delete_requested(const QStringList& filepaths);
    // User picked "Information..." in the right-click menu on a single row.
    // The controller (which owns the live plugin-db snapshot) opens the
    // dialog; SavesTab just hands off the row index.
    void information_requested(int row);
    // Lazy-scan trigger (Workspace-ugm3): emitted once when the tab is first
    // shown. The controller answers by building a scan request.
    void scan_requested();

protected:
    // Lazy load (Workspace-ugm3): the tab is created at game load but only
    // shown when the user clicks it - that first show fires the one scan.
    void showEvent(QShowEvent* event) override;
    // Drop the hover info panel when the pointer leaves or the window blurs.
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    // Streaming scan's final "done" signal: the worker emits entryReady per
    // save plus one finished(int count) at the end. We use this to flip
    // scanning_=false and drain any coalesced pending_request_.
    void on_scan_complete(int count);
    // Per-save streaming slot (Workspace-0owv). Connected to the worker's
    // entryReady signal with Qt::QueuedConnection; runs on the main thread.
    // Takes shared_ptr so the queued connection copies the pointer, not the
    // whole SaveGame (Workspace-3wgp).
    // `done` and `total` are the worker's progress counters (1-based and
    // total-enumerated, respectively). Each call inserts one row via binary
    // search on creation_time, so the table and saves_.entries stay sorted
    // newest-first without a final re-sort barrier.
    void on_entry_ready(std::shared_ptr<SavesScanResultEntry> entry, int done,
                        int total);
    void on_item_entered(QTableWidgetItem* item);
    void on_selection_changed();
    // Workspace-de5v: re-parse + cache the full save when heavy data was
    // stripped at scan time. No-op when already loaded.
    void ensure_heavy_data(int row);
    void show_save_info(int row);
    void hide_save_info();
    void on_delete_key();
    void on_information_action();
    void on_context_menu(const QPoint& pos);
    // Missing-column tooltip: plugin → provider summary (MO2 tooltip spirit).
    static QString missing_tooltip(const SavesScanResultEntry& entry);

    QTableWidget* table_ = nullptr;
    SavesScanThread* scan_thread_ = nullptr;
    SavesScanResult saves_;
    std::filesystem::path saves_dir_;
    bool scanning_ = false;
    // Lazy-scan latch (Workspace-ugm3): set on the first showEvent (or the
    // first ensure_scanned call); reset when the saves dir changes or the
    // list is cleared so the next game/instance scans on first show.
    bool scanned_once_ = false;
    // Coalesce: a scan request that arrives while one is in flight replaces
    // the latest pending request. Avoids the double-fire on boot (k53a) and
    // overlapping scans when the user mashes Refresh.
    std::optional<SavesScanRequest> pending_request_;
    // Hover info popup (MO2 GamebryoSaveGameInfoWidget port). Recreated per
    // hover so content never goes stale.
    QPointer<QWidget> info_popup_;
};

}  // namespace ui
