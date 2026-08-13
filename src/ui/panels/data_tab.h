#pragma once

#include "ui/main_window/data_tab_build_worker.h"

#include <QHash>
#include <QPoint>
#include <QVector>
#include <QWidget>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

class QMenu;
class QShowEvent;
class QTreeWidget;
class QTreeWidgetItem;

namespace ui {

struct ModEntry;
struct ConflictPairs;

class DataTab : public QWidget {
    Q_OBJECT
public:
    explicit DataTab(QWidget* parent = nullptr);
    ~DataTab() override;
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
    // Deferred population: show_data stores inputs and rebuilds the tree only
    // once the tab is actually visible (the RightPanel is a QTabWidget, so a
    // non-current page is hidden). The heavy per-file stat pass always runs on
    // the DataTabBuildThread; the main thread only fills QTreeWidget items.
    void showEvent(QShowEvent* event) override;

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

    // Queue a background population of the current view. Snapshot of the
    // stored inputs goes to the worker; the finished() row set is applied by
    // apply_build_result() on the main thread.
    void request_populate();
    void on_build_finished(DataTabBuildResult result, quint64 generation);
    void apply_build_result(DataTabBuildResult result);
    // Fill the next chunk of pending_rows_ into the tree (one event-loop turn
    // each) so the main thread stays responsive while a large instance's tree
    // materializes.
    void apply_chunk_step();
    DataTabBuildRequest build_request() const;

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

    // Population state machine. dirty_ is set when the stored inputs changed
    // (or the view switched) and cleared once a build is queued; showEvent
    // re-triggers it after a deferral. build_pending_ + pending_rows_ cover the
    // in-flight window: a build running on the worker thread, or its row set
    // still being chunked into the tree. The generation counter drops stale
    // results (the latest build always wins, like ConflictScanThread).
    bool dirty_ = true;
    bool build_pending_ = false;
    quint64 build_generation_ = 0;
    std::vector<DataTabRow> pending_rows_;
    std::vector<DataTabRow> applied_rows_;  // last fully applied row set (path-keyed no-op)
    std::size_t pending_pos_ = 0;

    // Display-path -> item index, kept in sync with tree_ so ensure_child is
    // O(1) instead of the linear child scan (the 11k-file O(n^2) rebuild cost).
    QHash<QString, QTreeWidgetItem*> item_index_;

    DataTabBuildThread* build_thread_ = nullptr;
    QTreeWidget* tree_ = nullptr;
};

}  // namespace ui
