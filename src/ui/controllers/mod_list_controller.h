#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "ui/main_window/main_window.h"

class QMenu;
class QVBoxLayout;

namespace ui {

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

public slots:
  void setup_mod_list_context_menu();
  void load_mods_from_game();
  void add_installed_mod(const std::string &folder_name);
  void update_status_bar_for_game();
  void sync_mod_enable_state(const QString &mod_id, bool enabled);
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
  void rename_mod_inline(int row); // start inline edit on a row's name cell
  void apply_rename(int row,
                    const QString &name); // model rename_requested handler
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
  // Conflict recompute (THREADING.md §3.6, P8.1) — see MainWindow comments.
  void recompute_conflicts();
  void request_conflict_scan(std::function<void()> follow_up);
  void start_conflict_scan();
  void on_conflict_scan_finished(ui::ConflictScanResult result,
                                 quint64 generation);
  void apply_conflict_results(const ui::ConflictScanResult &result);
  void
  launch_conflict_scan_batch(std::vector<std::function<void()>> follow_ups);
  ui::ConflictScanRequest build_conflict_scan_request();
  void reload_open_modinfo_dialog();
  // LOOT advisory-tool sort (PLAN.md §7.1).
  void run_loot_sort();
  void on_loot_progress(int stage, const QString &message);
  void on_loot_finished(engine::LootResult result);
  void refresh_data_tab();
  void wire_data_tab();
  void on_data_open(const QString &file_path);
  void on_data_execute(const QString &file_path, bool is_windows_exe,
                       const QString &vfs_path);
  void on_data_preview(const QString &file_path,
                       const QStringList &provider_paths,
                       const QStringList &provider_names);
  void on_data_add_executable(const QString &file_path,
                              const QString &default_name,
                              const QString &physical_path = {});
  void on_data_mod_info(const QString &mod_id, int initial_tab = -1);
  void on_data_hide(const QString &file_path, const QString &mod_id, bool hide);
  ui::ModInfoData build_mod_info_data(const ModEntry &mod);
  void on_image_diff_requested(const QString &relative_path);
  // Mod scan (THREADING.md §3.5/§3.6, P8.2).
  void on_mod_scan_finished(ui::ModScanResult result, quint64 generation);
  ui::ModScanRequest build_mod_scan_request();
  // Apply the active profile's modlist.txt enabled state to the UI model
  // after a scan. The scan result reflects the global on-disk disable.it
  // marker; the profile's modlist.txt is the per-profile source of truth.
  void apply_profile_mod_states();
  // Plugin-DB preload (THREADING.md §3.5, P8.5/T6).
  void launch_plugin_db_preload();
  void on_plugin_db_preloaded(engine::PluginDatabase db, quint64 generation);
  bool adopt_preloaded_plugin_db();
  void load_meta_for_mods();
  void restore_mod_column_visibility();
  // Plugins tab (Skyrim-style games with plugin support).
  void refresh_plugins_tab();
  void on_plugin_toggle(const std::string &name, bool enabled);
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
  [[nodiscard]] SourceVisitInfo
  source_visit_info(const QString &source_type, const QString &source_id,
                    const QString &page_url = {}) const;

  // Repopulate the profile selector from the current instance's profiles dir.
  // Resolves the profile to select: the current profile when it still exists,
  // else the saved default profile, else the first profile. Called on every
  // instance load and after the profile manager mutates the list.
  void refresh_profiles();

private:
  // Opens the profile manager dialog (MO2's ProfilesDialog). Applies the
  // selected profile switch and refreshes the selector on list changes.
  void open_profile_manager();
  // Applies a profile switch via engine::profile::switch_profile (g08): the
  // current profile is saved (modlist flush, plugins, archives, settings),
  // the new profile's state is restored, the UI views are refreshed through
  // the callbacks, and the P1.3 kProfileChanged event is dispatched by the
  // engine. On success the active profile name, window title and selector
  // are updated.
  void switch_profile(const QString &profile);

  // Recomputes "enabled / total" and updates the mod-list QLCDNumbers
  // (w_->mod_count_enabled_ / w_->mod_count_total_). Called on
  // dataChanged (toggles) and mod_list_changed (add/remove/move/load).
  void update_mod_count_label();

  MainWindow *w_ = nullptr;
};

} // namespace ui