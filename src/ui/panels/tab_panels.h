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
class QLCDNumber;
class QMenu;
class QProgressBar;
class QPushButton;
class QShowEvent;
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

    // MO2-style plugin counter (PluginListView::updatePluginCount parity,
    // modorganizer/src/pluginlistview.cpp:67). The number shows how many
    // enabled plugins pass the tab's text filter (row-hidden rows are
    // excluded); the tooltip breaks the count down by type with active/total
    // columns. Recomputed by set_plugins()/sync_enabled(), on show(), and by
    // RightPanel whenever the filter text changes.
    void refresh_counters();

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

    // User role on the Flags column holding the row's emblems as individual
    // QIcons (QList<QIcon>). A FlagsDelegate paints them one-by-one at native
    // size (wrapping, growing the row) - the stacked-pixmap single-icon
    // approach scales every emblem down to one icon slot.
    static constexpr int kPluginFlagsRole = Qt::UserRole + 60;
    // Parallel role on the Flags column: per-emblem hover text (QStringList,
    // same order as the kPluginFlagsRole icon list). FlagsDelegate::helpEvent
    // shows ONLY the entry of the emblem under the cursor.
    static constexpr int kPluginFlagTooltipsRole = Qt::UserRole + 61;

signals:
    void toggle_requested(const std::string& name, bool enabled);
    void reorder_requested(int from_row, int to_row);
    // User-pinned (immovable) load-order lock, from the row context menu.
    void lock_requested(const std::string& name, bool locked);
    // Refresh button pressed: re-scan plugins on disk and repopulate.
    void refresh_requested();

protected:
    // Fills `menu` with the actions for the row at `row` (MO2's
    // PluginListContextMenu lock actions). Split out of on_custom_context_menu
    // so tests can drive it without exec()-ing a modal menu (DataTab pattern).
    void add_context_menu_actions(QMenu& menu, int row);
    // Recompute the counter when the tab becomes visible again (the text
    // filter may have been changed while another tab was current).
    void showEvent(QShowEvent* event) override;

private:
    void apply_highlights();
    void on_custom_context_menu(const QPoint& pos);
    // Recompute every row's height from the emblem wrap math (FlagsDelegate
    // paints one QIcon per emblem; a QTableWidget does not auto-size rows from
    // a delegate's sizeHint). Runs after set_plugins and on Flags-column
    // resizes so wrapping rows grow as the column narrows.
    void relayout_flag_rows();

    // MO2 plugin classification for the counter (updatePluginCount order):
    // medium > light (ext-or-flag) > master (ext-or-flag) > regular.
    enum class PluginType { Regular, Master, Light, Medium };

    class PluginTable;
    PluginTable* table_ = nullptr;
    QPushButton* refresh_button_ = nullptr;
    QLCDNumber* counter_display_ = nullptr;
    std::vector<std::string> names_;
    // Per-row engine state backing the context menu (locked / core rows).
    std::vector<bool> rows_locked_;
    std::vector<bool> rows_force_loaded_;
    // Per-row MO2 plugin type, index-aligned with names_ (drives the counter).
    std::vector<PluginType> rows_type_;
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
    //   game_root_dir     - game install root (for the Root view's game-native
    //                       file walk; may be empty to disable the Root view)
    //   mods_subpath      - the game's data-dir name inside the root (Skyrim:
    //                       "Data"); used for the Root->Data nav label and to
    //                       exclude the game's own folders from the native walk
    //   deploy_prefix     - game-relative subpath mods deploy into when not
    //                       root-flagged; the data-dir segment a root-override
    //                       mod's content must keep (Skyrim: "Data")
    //   deploy_include_mod_id - true when mods deploy under <deploy_prefix>/
    //                       <mod_folder>/ (Isaac convention); the extra segment
    //                       the Add-as-Executable merged path must carry
    void show_data(
        const std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>& registry,
        const QVector<ModEntry>& all_mods,
        bool conflict_reversed,
        const std::filesystem::path& mods_dir,
        const std::filesystem::path& game_mods_dir,
        const std::filesystem::path& game_root_dir,
        const std::string& mods_subpath,
        const std::string& deploy_prefix,
        bool deploy_include_mod_id);

    // Incrementally merge one just-installed mod into the existing tree instead
    // of rebuilding it. Call after the conflict registry was recomputed to
    // include the new mod: files only it provides insert new rows, files it
    // shares with other mods bump their provider counts in place. Existing
    // items are updated, never recreated, so the cost scales with the new
    // mod's file count rather than the whole merged tree.
    void apply_mod(
        const std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>& registry,
        const std::string& mod_id,
        const QVector<ModEntry>& all_mods,
        bool conflict_reversed,
        const std::filesystem::path& mods_dir,
        const std::filesystem::path& game_mods_dir,
        const std::filesystem::path& game_root_dir,
        const std::string& mods_subpath,
        const std::string& deploy_prefix,
        bool deploy_include_mod_id);

    void clear_content();

    [[nodiscard]] QTreeWidget* tree_widget() const { return tree_; }

signals:
    // A non-executable file should be opened with its default handler.
    void open_requested(const QString& file_path);
    // An executable file should be executed: native binaries directly, .exe
    // through the instance's Proton/Wine runtime. Both routes mount the
    // overlay (merged view), so `vfs_path` (the merged Data-relative path,
    // DataVfsPathRole - empty for legacy absolute entries) is passed for the
    // receiver to resolve against the overlay-launch chain.
    void execute_requested(const QString& file_path, bool is_windows_exe,
                           const QString& vfs_path);
    // A previewable file should be shown in the preview window. provider_paths
    // / provider_names list the on-disk copies of every provider (primary
    // first) so the window can browse variants (MO2's PreviewDialog).
    void preview_requested(const QString& file_path,
                           const QStringList& provider_paths,
                           const QStringList& provider_names);
    // Register the file in the executables list (default name suggestion).
    // physical_path is the on-disk path of the winning copy (DataRealPathRole),
    // used by the receiver to resolve the exe's icon - it exists even when the
    // merged-view path does not (mod-provided executable before any deploy).
    void add_executable_requested(const QString& file_path,
                                  const QString& default_name,
                                  const QString& physical_path);
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

private:
    // The merged tree has two scopes: the game's data dir (default, shows the
    // mod-overlaid data content) and the game root (root-override mods' root
    // files + game-native root files like skse64_loader.exe). A top-level
    // navigation row (".." in the Data view, the data-dir folder in the Root
    // view) switches between them.
    enum class View { Data, Root };
    void switch_view(View v);
    // Rebuild the tree from the stored inputs (used on view switch).
    void rebuild_from_stored();

    // Stored show_data inputs so switch_view() can rebuild without the caller
    // re-supplying them.
    View view_ = View::Data;
    std::unordered_map<std::string, std::vector<std::pair<std::string, int>>> stored_registry_;
    QVector<ModEntry> stored_mods_;
    bool stored_conflict_reversed_ = false;
    std::filesystem::path stored_mods_dir_;
    std::filesystem::path stored_game_mods_dir_;
    std::filesystem::path stored_game_root_dir_;
    std::string stored_mods_subpath_;
    std::string stored_deploy_prefix_;
    bool stored_deploy_include_mod_id_ = false;

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
