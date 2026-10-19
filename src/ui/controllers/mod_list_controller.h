#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/source/nexus/update_check.h"
#include "ui/controllers/mod_actions.h"
#include "ui/controllers/backup_actions.h"
#include "ui/controllers/mod_context_menu.h"
#include "ui/main_window/main_window.h"

class QMenu;
class QVBoxLayout;

namespace ui {

class LootReportDialog;

// Visit-on-source-site info for a mod's Source column / context menu
// (label + URL). Resolved from the mod's source_type/source_id by
// ModListController::source_visit_info.
struct SourceVisitInfo {
  QString label;
  QString url;
};

// The mod list is the heart of the manager: loading the mods dir (background
// scan + plugin-DB preload), the conflict engine (debounced background scans),
// ordering/separators/filters, the Plugins and Data tabs, LOOT advisory sorts,
// mod info dialogs, and the mod-list context menu. Everything that mutates the
// model or reacts to its signals lives here; MainWindow (the composer) owns the
// widgets and wiring, and delegates mod-list behavior to this controller.
//
// Issue #16: split out of the 7211-line main_window.cpp.
class ModListController : public QObject {
  Q_OBJECT
public:
  explicit ModListController(MainWindow *w, QObject *parent = nullptr);

  // Builds the mod-list area inside `left_layout` (called from the
  // MainWindow ctor): model, view, header, filter connections, context menu.
  void setup_mod_list(QVBoxLayout *left_layout);

  // Maps a ModList column to the Mod Info tab a plain double-click should
  // open (MO2's modlistview.cpp double-click; GMM renames MO2's Nexus tab
  // to Source). Returns -1 for the last-used tab. GMM has no Notes/Game/
  // Mod-ID columns, so those MO2 mappings have no target here.
  [[nodiscard]] static int mod_info_tab_for_column(int column);

public slots:
  void setup_mod_list_context_menu();
  void load_mods_from_game();
  void add_installed_mod(const std::string &folder_name);
  // Scroll the just-installed mod into view and select it, opening the
  // separator band it sits in first if that band is folded. Navigation only -
  // it never moves the mod. MO2 ModListView::onModInstalled
  // (modlistview.cpp:465-478) does exactly this and nothing more.
  void focus_installed_mod(const std::string &folder_name);
  void update_status_bar_for_game();
  void sync_mod_enable_state(const QString &mod_id, bool enabled);
  // Repaint one mod's Version cell. Used when the version arrives AFTER the
  // row was added - the install path fetches it from Nexus once the row is
  // already on screen, so the row is refreshed in place rather than rebuilt.
  void set_mod_version(const QString &mod_id, const QString &version);
  void sync_priorities();
  void sort_mods();
  void create_separator();
  QString create_separator_named(const QString &name, const QString &color);
  void create_empty_mod();
  void import_archives(const QStringList &paths);
  void export_modlist();
  void import_modlist();
  void open_folder(ui::FolderKind kind);
  void create_separator_at_row(int row);
  void rename_mod_inline(int row);  // start inline edit on a row's name cell
  void apply_rename(int row,
                    const QString &name);  // model rename_requested handler
  void delete_separator(int row);
  void select_color_for_selected();
  void reset_color_for_selected();
  void save_order();
  void load_order();
  void sync_separator_ids();
  // Persist per-mod UI state (folded + parent_id) to the manager sidecar
  // ({instance_root}/meta/{folder_name}.ini, [GameModManager] section).
  // Runs on every mod_list_changed (fold toggles, nesting drops, renames,
  // deletes) so the sidecar always mirrors the model. Pseudo-rows
  // (Overwrite/MERGED/game-native) never persist fold/parent.
  void sync_mod_ui_state();
  void group_mods_by_separator();
  void apply_mod_filter();
  // Conflict recompute (THREADING.md §3.6, P8.1) - see MainWindow comments.
  void recompute_conflicts();
  void request_conflict_scan(std::function<void()> follow_up);
  void start_conflict_scan();
  void on_conflict_scan_finished(ui::ConflictScanResult result, quint64 generation);
  void apply_conflict_results(const ui::ConflictScanResult &result);
  void launch_conflict_scan_batch(std::vector<std::function<void()>> follow_ups);
  ui::ConflictScanRequest build_conflict_scan_request();
  void reload_open_modinfo_dialog();
  // Nexus mod-update sweep (Workspace-sc4z). The DRIVER for
  // engine::Source::Nexus::UpdateCheck::run(): resolves the game's Nexus
  // domain + the stored API key on the main thread, runs the sweep on
  // NexusUpdateThread, and reports the outcome into the status bar's
  // persistent update notice. Manual by design - see the MainWindow comment.
  //
  // `force` asks Nexus about every mod instead of trusting the per-mod
  // lastnexusquery cache, at one request per mod. The toolbar button passes
  // true because a stamp left behind by a sweep that never got an answer is
  // indistinguishable from a real one on disk, and would otherwise suppress
  // that mod's update forever with no way for the user to clear it.
  void check_mod_updates(bool force = false);
  void on_nexus_update_finished(engine::Source::Nexus::UpdateCheck::Report report,
                                quint64 generation);
  // Silence (or un-silence) the newest Nexus version for ONE mod. The write is
  // the engine's (per-mod meta.ini, never per-profile); this only routes it
  // and re-reads the metas so the row's badge follows what was just written.
  void set_mod_update_ignored(const QString &mod_id, bool ignore);
  // LOOT advisory-tool sort (PLAN.md §7.1).
  void run_loot_sort();
  void on_loot_progress(int stage, const QString &message);
  void on_loot_finished(engine::Sorter::Loot::Result result);
  void refresh_data_tab();
  void wire_data_tab();
  void on_data_open(const QString &file_path);
  void on_data_execute(const QString &file_path, bool is_windows_exe,
                       const QString &vfs_path);
  void on_data_preview(const QString &file_path, const QStringList &provider_paths,
                       const QStringList &provider_names);
  void on_data_add_executable(const QString &file_path, const QString &default_name,
                              const QString &physical_path = {});
  void on_data_mod_info(const QString &mod_id, int initial_tab = -1);
  void on_data_hide(const QString &file_path, const QString &mod_id, bool hide);
  ui::ModInfoData build_mod_info_data(const ModEntry &mod);
  void on_image_diff_requested(const QString &relative_path);
  // ConflictsTab double-click (Workspace-co2): OS-open or built-in preview
  // of a conflicted file, resolved to the owning mod's on-disk copy.
  void on_conflict_file_open(const QString &mod_id, const QString &relative_path);
  void on_conflict_file_preview(const QString &mod_id, const QString &relative_path);
  // Ctrl+double-click in the Conflicts tab: reveal the file's containing
  // folder in the OS file manager.
  void on_conflict_file_reveal(const QString &mod_id, const QString &relative_path);
  // Mod scan (THREADING.md §3.5/§3.6, P8.2).
  void on_mod_scan_finished(ui::ModScanResult result, quint64 generation);
  ui::ModScanRequest build_mod_scan_request();
  // Apply the active profile's modlist.txt enabled state to the UI model
  // after a scan. The scan result reflects the global on-disk disable.it
  // marker; the profile's modlist.txt is the per-profile source of truth.
  void apply_profile_mod_states();
  // Write the active profile's enabled state through to the on-disk disable
  // sentinel, which is the deploy's only input (deploy_utils.cpp:389). Runs on
  // a profile switch for every game that does not defer the write. A mod the
  // profile does not list is disabled, per MO2; the sentinel is read first and
  // only a mod whose state actually moved is written. See the definition.
  void apply_profile_deploy_state();
  // Plugin-DB preload (THREADING.md §3.5, P8.5/T6).
  void launch_plugin_db_preload();
  void on_plugin_db_preloaded(engine::PluginDb::Database db, quint64 generation);
  bool adopt_preloaded_plugin_db();
  void load_meta_for_mods();
  void restore_mod_column_visibility();
  // Plugins tab (Skyrim-style games with plugin support).
  //
  // `write_back` says whether this refresh may PERSIST state it derived back
  // into the profile files: the first-run heal (no profile yet) and the
  // native/CC band repair both do, because a normal load must leave a coherent
  // profile on disk. false makes the refresh a pure read-and-push, for the
  // post-restore reload in BackupActions - there, "the profile on disk cannot
  // be applied" is exactly the state the restore left behind, and writing would
  // silently undo the "not written" the report just promised about it.
  void refresh_plugins_tab(bool write_back = true);
  // Repopulate the right panel's Archives tab from the enabled plugin files.
  void refresh_archives_tab();
  // Push the current mod selection's conflicts to the ConflictsTab.
  // No-op while the tab is still a lazy placeholder (Workspace-j6ty); the
  // tab_materialized handler re-runs it once the tab is built.
  void refresh_conflicts_tab();
  void on_plugin_toggle(const std::string &name, bool enabled);
  void on_plugin_set_all(bool enabled);
  void on_plugin_reorder(int from_row, int to_row);
  void on_plugin_lock(const std::string &name, bool locked);
  void on_mod_selection_changed();
  void on_plugin_selection_changed();
  void rebuild_plugin_highlight_index();
  // Context-menu actions.
  void remove_selected_mods();
  void move_to_separator(const QString &mod_id, const QString &sep_id);
  void send_to_separator(const QString &mod_id);
  void send_to_highest_priority(const QString &id);
  void send_to_lowest_priority(const QString &id);
  void send_to_highest_in_separator(const QString &id);
  void send_to_lowest_in_separator(const QString &id);
  void send_to_first_conflict(const QString &id);
  void send_to_last_conflict(const QString &id);
  void priority_move_selected(int step);
  void toggle_selected_mods(bool enabled);
  // "Treat mod as root dir" (Tweaks menu).
  void toggle_root_override(const QList<int> &rows, bool on);
  // MO2's "Change Categories" (checkable) + "Primary Category" (radio)
  // submenus for a single mod. Both edit the mod's [General] "category" CSV
  // (primary first) in the manager sidecar meta; every change persists
  // immediately and refreshes the mod list filter.
  void add_category_menus(QMenu &menu, const QString &mod_id);

  // Nexus game domain for the current game ("skyrimspecialedition"), resolved
  // from the loaded plugin's identity - the single source of truth (there is
  // NO "nexus_domain" knowledge hook; plugins register it via
  // register_identity).
  [[nodiscard]] QString current_nexus_domain() const;
  [[nodiscard]] SourceVisitInfo source_visit_info(const QString &source_type,
                                                  const QString &source_id,
                                                  const QString &page_url = {}) const;

  // Opens the profile manager dialog (MO2's ProfilesDialog, Ctrl+P). Applies
  // the selected profile switch and refreshes the selector on list changes.
  void open_profile_manager();

  // Opens the selected mod's page on the site it came from (MO2's Ctrl+N,
  // named for the source rather than for a vendor). Takes the first selected
  // row; does nothing when nothing is selected or the row has no source, so
  // the key never opens a blank page.
  void visit_selected_mod_source();

  // Repopulate the profile selector from the current instance's profiles dir.
  // Resolves the profile to select: the current profile when it still exists,
  // else the saved default profile, else the first profile. Called on every
  // instance load and after the profile manager mutates the list.
  void refresh_profiles();

  // Applies a profile switch via engine::profile::switch_profile (g08): the
  // current profile is saved (modlist flush, plugins, archives, settings),
  // the new profile's state is restored, the UI views are refreshed through
  // the callbacks, and the P1.3 kProfileChanged event is dispatched by the
  // engine. On success the active profile name, window title and selector
  // are updated. Public because MainWindow forwards the -p command line flag
  // here at startup.
  void switch_profile(const QString &profile);

private:
  // Recomputes the enabled-mod count and updates the mod-list QLCDNumber
  // (w_->mod_count_enabled_). Called on dataChanged (toggles) and
  // mod_list_changed (add/remove/move/load).
  void update_mod_count_label();

  // Resolve a conflicted file to the owning mod's on-disk copy (instance
  // mods dir first, then the game-native mods dir). Empty when neither
  // copy exists. Helper for the ConflictsTab open/preview receivers.
  QString conflict_file_abs_path(const QString &mod_id,
                                 const QString &relative_path) const;

  MainWindow *w_ = nullptr;

  // Extracted sub-controllers for mod actions and context menu.
  std::unique_ptr<ModActions> mod_actions_;
  std::unique_ptr<ModContextMenu> mod_context_menu_;
  // Backup / restore points for the load order and the mod list
  // Owns the four toolbar actions and their dialogs; the
  // engine does the file work.
  std::unique_ptr<BackupActions> backup_actions_;

  // MO2's LOOT report popup (loot.cpp:960-984 runLoot opens the LootDialog
  // when the sort starts). Created when a sort starts, filled when it ends,
  // deleted by the user closing it - QPointer so a closed popup goes null
  // instead of dangling.
  QPointer<LootReportDialog> loot_dialog_;

  // True only while the gated LockUI lock taken by run_loot_sort() is actually
  // engaged, i.e. while Settings::lock_gui() is on. Guards the matching
  // unlock_ui() in on_loot_finished: the Locker is shared with the install
  // paths and its unlock() is unconditional, so unlocking a lock our own call
  // declined would lift theirs.
  bool loot_locked_ = false;
};

}  // namespace ui