#pragma once

#include "engine/game/saves/save_game.h"
#include "engine/game/saves/save_missing_assets.h"
#include "ui/main_window/saves_scan_worker.h"

#include <QPointer>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <filesystem>
#include <optional>
#include <vector>

class QTableWidget;
class QTableWidgetItem;
class QEvent;

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
    // Scans are NOT watched: one runs at game load, and MainWindow re-runs one
    // after a delete. No background re-scans.
    void set_saves_dir(const std::filesystem::path& dir);
    [[nodiscard]] std::filesystem::path saves_dir() const { return saves_dir_; }

    // Game/instance switch: drop the current list.
    void clear_saves();

    // Run one scan on the background thread. The request carries a snapshot of
    // the current load order + dirs, so results reflect the state at the moment
    // the refresh was asked for. MainWindow builds it in answer to a delete
    // (or the initial fill at game load).
    void request_scan(SavesScanRequest request);

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

signals:
    // Delete the named save files (and their .skse co-saves) - MainWindow
    // routes through engine::remove_path (trash), then re-scans.
    void delete_requested(const QStringList& filepaths);
    // User picked "Information..." in the right-click menu on a single row.
    // The controller (which owns the live plugin-db snapshot) opens the
    // dialog; SavesTab just hands off the row index.
    void information_requested(int row);

protected:
    // Drop the hover info panel when the pointer leaves or the window blurs.
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    // Streaming scan's final "done" signal: the worker emits entryReady per
    // save plus one finished(int count) at the end. We use this to flip
    // scanning_=false and drain any coalesced pending_request_.
    void on_scan_complete(int count);
    // Per-save streaming slot (Workspace-0owv). Connected to the worker's
    // entryReady signal with Qt::QueuedConnection; runs on the main thread.
    // `done` and `total` are the worker's progress counters (1-based and
    // total-enumerated, respectively). Each call inserts one row via binary
    // search on creation_time, so the table and saves_.entries stay sorted
    // newest-first without a final re-sort barrier.
    void on_entry_ready(SavesScanResultEntry entry, int done, int total);
    void on_item_entered(QTableWidgetItem* item);
    void on_selection_changed();
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
    // Coalesce: a scan request that arrives while one is in flight replaces
    // the latest pending request. Avoids the double-fire on boot (k53a) and
    // overlapping scans when the user mashes Refresh.
    std::optional<SavesScanRequest> pending_request_;
    // Hover info popup (MO2 GamebryoSaveGameInfoWidget port). Recreated per
    // hover so content never goes stale.
    QPointer<QWidget> info_popup_;
};

}  // namespace ui
