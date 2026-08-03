#pragma once

#include "engine/plugins/plugin_info.h"

#include <QPoint>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class QCheckBox;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QFileSystemWatcher;
class QMenu;
class QProgressBar;
class QTableWidget;
class QTableWidgetItem;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace ui {

struct ModEntry;
struct ConflictPairs;

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
                      const std::string& parent_mod_id = {});
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
                           const std::string& display_name);
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
        QTableWidgetItem* name_item = nullptr;
        QTableWidgetItem* source_item = nullptr;
        QTableWidgetItem* size_item = nullptr;
        QProgressBar* progress_bar = nullptr;
    };

    DownloadEntry& entry_for(const std::string& id);
    void replace_bar_with_label(const std::string& id, const QString& text,
                                const QColor& bg, const QColor& fg);
    void on_cell_double_clicked(int row, int column);
    void on_custom_context_menu(const QPoint& pos);
    void remove_entry(const std::string& id);
    void apply_installed_filter();

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
    bool has_active_download() const;

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
    std::filesystem::path downloads_dir_;
    ConflictResolver conflict_resolver_;
    QFileSystemWatcher* dir_watcher_ = nullptr;
    QTimer* scan_timer_ = nullptr;
};

class PluginsTab : public QWidget {
    Q_OBJECT
public:
    explicit PluginsTab(QWidget* parent = nullptr);
    // Out-of-line: table_ is a private PluginTable* whose base needs the
    // complete type for the upcast.
    [[nodiscard]] QTableWidget* table() const;

    // Replace the plugin list contents. Row 0 = most dominant (first-loaded).
    // Force-loaded rows (game-native, CC) are pinned and shown greyed.
    void set_plugins(const std::vector<engine::GamePlugin>& plugins);

    // Re-sync enabled checkboxes from engine state without rebuilding rows
    // (used to revert a blocked toggle, incl. transitively flipped masters).
    void sync_enabled(const std::vector<engine::GamePlugin>& plugins);

    // MO2 parity. Two independent highlight flags rendered by apply_highlights
    // (contained wins over master), re-applied by set_plugins() which rebuilds
    // the rows:
    //  - set_contained_plugins: plugins owned by the mod selected in the mod
    //    list -> plugin_list_contained.
    //  - set_master_plugins: masters of the plugin selected here ->
    //    plugin_list_master.
    void set_contained_plugins(const QVector<QString>& contained);
    void set_master_plugins(const QVector<QString>& masters);

    // Names of the plugins currently selected in the table (row order).
    [[nodiscard]] QStringList selected_plugin_names() const;

signals:
    void toggle_requested(const std::string& name, bool enabled);
    void reorder_requested(int from_row, int to_row);

private:
    void apply_highlights();

    class PluginTable;
    PluginTable* table_ = nullptr;
    std::vector<std::string> names_;
    QSet<QString> contained_names_;
    QSet<QString> master_names_;
    bool syncing_ = false;
};

class ArchivesTab : public QWidget {
    Q_OBJECT
public:
    explicit ArchivesTab(QWidget* parent = nullptr);
    [[nodiscard]] QTreeWidget* tree() const { return tree_; }
private:
    QTreeWidget* tree_ = nullptr;
};

class DataTab : public QWidget {
    Q_OBJECT
public:
    explicit DataTab(QWidget* parent = nullptr);
    [[nodiscard]] QTreeWidget* tree() const { return tree_; }

    // Populate the merged game-visible file tree from the conflict registry.
    //   registry          - relative path -> (mod_id, priority) providers
    //   all_mods          - current mod list (for display names)
    //   conflict_reversed - true if lower priority wins (Isaac convention)
    //   mods_dir          - instance mods dir (first place to stat winners)
    //   game_mods_dir     - game-native mods dir fallback (may be empty)
    void show_data(
        const std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>& registry,
        const QVector<ModEntry>& all_mods,
        bool conflict_reversed,
        const std::filesystem::path& mods_dir,
        const std::filesystem::path& game_mods_dir);

    void clear_content();

    [[nodiscard]] QTreeWidget* tree_widget() const { return tree_; }

signals:
    // A non-executable file should be opened with its default handler.
    void open_requested(const QString& file_path);
    // An executable file should be executed: native binaries directly, .exe
    // through the instance's Proton/Wine runtime (no VFS - plain execution).
    void execute_requested(const QString& file_path, bool is_windows_exe);
    // A previewable file should be shown in the preview window. provider_paths
    // / provider_names list the on-disk copies of every provider (primary
    // first) so the window can browse variants (MO2's PreviewDialog).
    void preview_requested(const QString& file_path,
                           const QStringList& provider_paths,
                           const QStringList& provider_names);
    // Register the file in the executables list (default name suggestion).
    void add_executable_requested(const QString& file_path,
                                  const QString& default_name);
    // Open the minimal Mod Info dialog for the mod owning the file.
    void open_mod_info_requested(const QString& mod_id);
    // Hide (rename to .gmmhidden) or un-hide the file on disk. mod_id is the
    // winner mod the file lives in - the caller drops that mod's conflict
    // cache entry (the quick token can't see renames inside subdirectories)
    // before re-computing the registry. The tree is rebuilt by the caller
    // after the registry is re-computed.
    void hide_requested(const QString& file_path, const QString& mod_id, bool hide);
    // Re-populate the tree from the current conflict registry.
    void refresh_requested();

protected:
    // Context-menu / double-click internals. Protected (not private) so tests
    // can drive the menu actions and open/preview paths directly.
    void on_custom_context_menu(const QPoint& pos);
    void on_item_double_clicked(QTreeWidgetItem* item, int column);
    void add_file_menus(QMenu& menu, QTreeWidgetItem* item);
    void add_common_menus(QMenu& menu);
    void open_item(QTreeWidgetItem* item);
    void preview_item(QTreeWidgetItem* item);
    void dump_tree_to_file();

    QTreeWidget* tree_ = nullptr;
};

class SavesTab : public QWidget {
    Q_OBJECT
public:
    explicit SavesTab(QWidget* parent = nullptr);
    [[nodiscard]] QTableWidget* table() const { return table_; }
private:
    QTableWidget* table_ = nullptr;
};

class ConflictsTab : public QWidget {
    Q_OBJECT
public:
    explicit ConflictsTab(QWidget* parent = nullptr);

    void show_conflicts(
        const QString& selected_mod_id,
        const QVector<ModEntry>& all_mods,
        const std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>& file_registry,
        const QMap<QString, ConflictPairs>& pairs,
        bool conflict_reversed);

    void clear_content();

signals:
    void file_open_requested(const QString& mod_id, const QString& relative_path);
    void image_diff_requested(const QString& relative_path);

private:
    void on_item_double_clicked(QTreeWidgetItem* item, int column);
    void on_custom_context_menu(const QPoint& pos);
    void on_merge_in_imagediff();

    QTreeWidget* tree_ = nullptr;
    QString context_file_path_;
};

}  // namespace ui
