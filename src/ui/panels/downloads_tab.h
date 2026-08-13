#pragma once

#include <QColor>
#include <QPoint>
#include <QString>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

class QCheckBox;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QFileSystemWatcher;
class QMenu;
class QProgressBar;
class QShowEvent;
class QTableWidget;
class QTableWidgetItem;
class QTimer;

namespace ui {

enum class DownloadState {
    Downloading,
    Paused,
    Complete,
    Installed,
    Failed,
    // Not implemented yet: reserved so manifests and rendering stay stable.
    Removed
};

// How a dropped archive that collides with an existing file in the downloads
// dir should be handled. The default resolver shows an MO2-style question
// dialog; tests inject a stub.
enum class DropConflictAction {
    Overwrite,
    Rename,
    Ignore
};

class DownloadsTab : public QWidget {
    Q_OBJECT
public:
    explicit DownloadsTab(QWidget* parent = nullptr);
    [[nodiscard]] QTableWidget* table() const { return table_; }

    void add_download(const std::string& id, const std::string& name,
                      const std::string& source,
                      const std::filesystem::path& file_path = {},
                      const std::string& nexus_domain = {},
                      int file_id = 0,
                      const std::string& parent_mod_id = {},
                      const std::string& page_url = {});
    void update_progress(const std::string& id, int64_t downloaded,
                         int64_t total, double speed);
    void mark_complete(const std::string& id, bool success);
    void mark_installed(const std::string& id);
    void mark_paused(const std::string& id);
    void mark_downloading(const std::string& id);

    // Update the displayed name of a download entry (e.g. the real mod name,
    // resolved from the source after the download was queued with a
    // placeholder). A no-op if the id is not present.
    void rename_download(const std::string& id, const std::string& new_name);

    // Record the on-disk archive for a download that was started without one
    // (e.g. Nexus downloads, whose path is only known after the fetch).
    void set_file_path(const std::string& id, const std::filesystem::path& path);

    // Directory the instance downloads archives land in (for "Show in Folder"
    // when an entry has no file yet).
    void set_downloads_dir(const std::filesystem::path& dir);

    // True while any entry is still downloading or paused, so the app can warn
    // (and the tab can guard rescanning) while a fetch is in flight.
    bool has_active_download() const;

    // Remember the shared filter bar's current text so "hide installed"
    // re-applications never unhide rows the text filter hid. Called by
    // RightPanel::apply_filter on every filter change/tab switch.
    void set_filter_text(const QString& text);

    // Re-apply the "hide installed" filter on top of any other row filter.
    void reapply_installed_filter();

    // Re-read the compact-downloads setting and set explicit row heights
    // (MO2 standard/compact) so the look does not depend on any stylesheet.
    void apply_compact_style();

    // Replace the conflict resolver shown when a dropped archive's name
    // collides with an existing file in the downloads dir. Defaults to the
    // MO2-style question dialog; callers (tests) may inject a stub.
    using ConflictResolver = std::function<DropConflictAction(
        const std::filesystem::path& existing_file,
        const std::filesystem::path& dropped_file)>;
    void set_conflict_resolver(ConflictResolver resolver);

    // Persistence
    [[nodiscard]] std::string serialize() const;
    void deserialize(const std::string& json,
                     const std::filesystem::path& downloads_dir);

signals:
    void install_requested(const std::string& id,
                           const std::filesystem::path& file_path,
                           const std::string& source_type,
                           const std::string& source_id,
                           int file_id,
                           const std::string& display_name,
                           const std::string& page_url = {});
    void pause_requested(const std::string& id);
    void resume_requested(const std::string& id);
    // Emitted with the raw pasted URL when the user triggers "Add from URL"
    // (LoversLab and other no-API sites). MainWindow validates and routes it.
    void loverslab_url_entered(const std::string& url);
    // Emitted after a download entry (and its file) has been removed, so the
    // manifest can be persisted. The entry is already gone from the table.
    void entry_removed(const std::string& id);

private slots:
    // The downloads dir changed on disk (watcher fired): (re)arm the debounce
    // timer so bursts from large copies coalesce into a single scan.
    void on_downloads_dir_changed();
    void on_scan_timer_timeout();

private:
    struct DownloadEntry {
        int row = -1;
        std::filesystem::path file_path;
        DownloadState state = DownloadState::Downloading;
        int64_t total_size = 0;
        // Origin metadata (Nexus): parent mod page id, file id, domain.
        std::string parent_mod_id;
        int file_id = 0;
        std::string nexus_domain;
        std::string category;
        // Source page URL (LoversLab: the download link minus the
        // ?do=download query). Persisted in the manifest so the "Open on ..."
        // context action and install provenance survive restarts.
        std::string page_url;
        QTableWidgetItem* name_item = nullptr;
        QTableWidgetItem* source_item = nullptr;
        QTableWidgetItem* size_item = nullptr;
        QProgressBar* progress_bar = nullptr;
    };

    DownloadEntry& entry_for(const std::string& id);
    void replace_bar_with_label(const std::string& id, const QString& text,
                                const QColor& bg, const QColor& fg);
    void on_cell_double_clicked(int row, int column);
    void remove_entry(const std::string& id);
    void apply_installed_filter();

    // Derive the origin metadata for an install from a download entry:
    // source_type ("nexus"/"loverslab"/""), source_id, file_id, and the
    // source page URL. LoversLab rows key off the entry id (the file id) and
    // carry page_url; Nexus rows carry parent_mod_id/file_id. Mirrors the
    // Source column's literal strings (not tr()).
    struct SourceInfo {
        std::string source_type;
        std::string source_id;
        int file_id = 0;
        std::string page_url;
    };
    SourceInfo source_info_for(const std::string& id,
                               const DownloadEntry& entry) const;

protected:
    // Fills `menu` with the actions for the download entry at `id` (install,
    // pause/resume, show in folder, source-aware "Open on ...", remove).
    // Split out of on_custom_context_menu so tests can drive it without
    // exec()-ing a modal menu (DataTab/PluginsTab pattern).
    void add_context_menu_actions(QMenu& menu, const std::string& id);

private:
    void on_custom_context_menu(const QPoint& pos);

    // Move or copy a dropped local archive into downloads_dir_ and surface it
    // as a "Manual" Complete entry. Returns true if an entry was added.
    bool import_dropped_file(const std::filesystem::path& source, bool move);

    // Add a download entry for a file already sitting in downloads_dir_
    // (e.g. a drop that resolves to the same location, or one moved/copied in
    // by import_dropped_file). Returns true if an entry was added. Adds
    // nothing if the file is not an archive or already backs a tracked entry.
    bool add_downloads_dir_file(const std::filesystem::path& path);

    // Add untracked archives sitting in the downloads dir as "Manual"
    // Complete entries so they can be installed from the tab. Refresh the size
    // of tracked same-named files and remove rows whose archive no longer
    // exists. Skip files that already back a tracked entry and any scan while
    // a download is in flight (the in-progress archive would otherwise appear
    // as a bogus "Complete" row).
    void scan_downloads_dir();

    // MO2 standard/compact row height in pixels, derived from the current font
    // so text never clips. Compact ~ font + 8; standard ~ font + 22.
    int row_height() const;

protected:
    void showEvent(QShowEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

    QTableWidget* table_ = nullptr;
    QCheckBox* hide_installed_ = nullptr;
    std::unordered_map<std::string, DownloadEntry> downloads_;
    // Last filter text passed by RightPanel::apply_filter (trimmed, lowered),
    // re-applied by apply_installed_filter so it never unhides rows the text
    // filter hid. Empty when no text filter is active.
    QString current_filter_text_;
    std::filesystem::path downloads_dir_;
    ConflictResolver conflict_resolver_;
    QFileSystemWatcher* dir_watcher_ = nullptr;
    QTimer* scan_timer_ = nullptr;
};

}  // namespace ui
