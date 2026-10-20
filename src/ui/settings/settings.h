#pragma once

#include <QColor>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QMessageBox>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <optional>

// App-level settings facade. All QSettings access goes through this class so
// keys stay centralized and typed, mirroring MO2's Settings singleton.
// Writes are immediate (per-control on change).
//
// The sections below are MO2's settings CLASSES, in MO2's own declaration order
// in references/modorganizer/src/settings.h, and each is named for the MO2 class
// it mirrors. They are sections in the header only - the flat facade, the one
// QSettings member and every accessor name are unchanged. What each section
// still carries is its current ini group, stated in its banner, and where that
// group differs from the MO2 key the later namespacing stage moves it and adds
// the migration row; settings_migration.{h,cpp} is that machinery. The group a
// key sits in is set by the key STRING in the .cpp, never by this layout.
//
// One deliberate divergence, on purpose: MO2's class split is NOT an ini split.
// Only GeometrySettings and WidgetSettings get an ini group of their own there;
// InterfaceSettings, ColorSettings, NexusSettings, NetworkSettings, GameSettings
// and DiagnosticsSettings all write into the same "Settings/" group. What is
// adopted here is the CLASS layout and the naming, not MO2's group names.
class Settings {
public:
  static Settings &instance();

  // == GameSettings =========================================================
  // MO2's GameSettings, renamed InstanceSettings per the vendor-to-capability
  // rule: MO2 is organised around one managed game, GMM around the managed
  // INSTANCE (several, each with its own game), so a class named after the game
  // would import an assumption this codebase does not have. Same rule renames
  // NexusSettings/SteamSettings to SourceSettings below.
  // Keys still live in game/ and profiles/; both move to instance/.
  // Default (startup) profile name for the current instance. Empty when no
  // default was chosen - the app then falls back to the first profile.
  QString default_profile() const;  // key: profiles/default
  void set_default_profile(const QString &name);
  bool force_enable_core_files() const;
  void set_force_enable_core_files(bool on);

  // == GeometrySettings =====================================================
  // MO2 GeometrySettings. MO2 keys geometry by widget objectName; GMM's
  // explicit names are clearer and immune to objectName churn, so the NAMES
  // stay. They move under geometry/ when this group is namespaced.
  bool center_dialogs() const;
  void set_center_dialogs(bool on);
  // Last wizard geometry + splitter states (empty until the first install).
  QByteArray fomod_window_geometry() const;  // key: fomod/window_geometry
  void set_fomod_window_geometry(const QByteArray &g);
  QByteArray fomod_center_split() const;  // key: fomod/center_split
  void set_fomod_center_split(const QByteArray &s);
  QByteArray fomod_left_split() const;  // key: fomod/left_split
  void set_fomod_left_split(const QByteArray &s);
  // Mod Info dialog geometry (restored on next open).
  QByteArray modinfo_window_geometry() const;  // key: modinfo/window_geometry
  void set_modinfo_window_geometry(const QByteArray &g);
  // ListDialog (generic choice picker) geometry - restored on next open.
  QByteArray listdialog_window_geometry() const;  // key: listdialog/window_geometry
  void set_listdialog_window_geometry(const QByteArray &g);
  // SaveInfoDialog (right-click save > Information) geometry - restored on next open.
  QByteArray saveinfo_window_geometry() const;  // key: saveinfo/window_geometry
  void set_saveinfo_window_geometry(const QByteArray &g);
  // Calls the four dialog-geometry setters above, so it keeps working whichever
  // keys they are stored under.
  void reset_dialog_geometry();

  // == WidgetSettings =======================================================
  // MO2 WidgetSettings, the newest of MO2's classes: state that stays valid
  // across display changes. WidgetSettings is MO2's home for exactly the class
  // of thing the per-instance mod-list state below is, and MO2 already scopes
  // its Widgets/ keys by widget, so scoping by instance is the same idea.
  // Keys still live in dialog_choices/, modinfo/, modlist/; all move to widgets/.
  // Remembered TaskDialog answers (MO2 QuestionBoxMemory equivalent).
  // dialog_choice returns the stored StandardButton for (action, file), or
  // nullopt when the dialog must ask. Keys live under dialog_choices/.
  std::optional<QMessageBox::StandardButton> dialog_choice(const QString &action,
                                                           const QString &file) const;
  void set_dialog_choice(const QString &action, const QString &file,
                         QMessageBox::StandardButton button);
  void reset_dialog_choices();
  // Mod Info tab order (MO2 GeometrySettings::modInfoTabOrder - a row of theirs
  // we already match; WidgetSettings is the better home for it here). The saved
  // value is the visual tab sequence as ModInfoTabId integers, so a tab added
  // in a later release is appended in its natural place rather than lost.
  QList<int> modinfo_tab_order() const;  // key: modinfo/tab_order
  void set_modinfo_tab_order(const QList<int> &order);
  int modinfo_last_tab() const;  // key: modinfo/last_tab
  void set_modinfo_last_tab(int index);
  // Per-instance set of mod-list columns the user has hidden (by column
  // name, stable across reordering). Missing key = first-run defaults apply.
  // The Name column is never stored here (it cannot be hidden).
  QStringList modlist_hidden_columns(const QString &instance_name) const;
  void set_modlist_hidden_columns(const QString &instance_name,
                                  const QStringList &hidden);
  // Write-once initialization for a newly created instance: seeds the
  // default hidden columns (Category, Source, Source ID, Installation,
  // Changed) only when no per-instance key exists yet. Existing saved
  // preferences are never overwritten.
  void ensure_modlist_column_defaults(const QString &instance_name);
  // Per-instance "nested mod list" toggle: allows dragging mods onto mods and
  // separators onto separators to build a visual nesting (indented, foldable
  // children) without changing load order / priorities. Defaults to off.
  bool modlist_nested(const QString &instance_name) const;
  void set_modlist_nested(const QString &instance_name, bool on);

  // == ColorSettings ========================================================
  // MO2 ColorSettings. KEYS ALREADY MATCH - this group is final and does not
  // move. MO2 names these Settings/overwrittenLooseFilesColor,
  // General/previousSeparatorColor and four others in camelCase; GMM's
  // snake_case names carry the same meaning under the same colors/ prefix.
  // Adopting MO2's spelling would be nine key renames with zero user benefit
  // and nonzero migration risk. Declined, permanently.
  bool color_separator_scrollbar() const;
  void set_color_separator_scrollbar(bool on);
  // Remembered color for the next separator (MO2 previousSeparatorColor).
  // Hidden setting - never shown in the settings dialog.
  std::optional<QColor> previous_separator_color() const;
  void set_previous_separator_color(const QColor &c);
  void remove_previous_separator_color();
  // MO2 mod-list conflict colors; defaults match MO2's colortable.cpp.
  QColor modlist_overwritten_loose() const;  // "Is overwritten (loose files)"
  void set_modlist_overwritten_loose(const QColor &c);
  QColor modlist_overwriting_loose() const;  // "Is overwriting (loose files)"
  void set_modlist_overwriting_loose(const QColor &c);
  QColor modlist_overwritten_archive() const;  // "Is overwritten (archives)"
  void set_modlist_overwritten_archive(const QColor &c);
  QColor modlist_overwriting_archive() const;  // "Is overwriting (archives)"
  void set_modlist_overwriting_archive(const QColor &c);
  QColor modlist_contains_file() const;  // "Mod contains selected file"
  void set_modlist_contains_file(const QColor &c);
  QColor plugin_list_contained() const;  // "Plugin is contained in selected mod"
  void set_plugin_list_contained(const QColor &c);
  QColor plugin_list_master() const;  // "Plugin is master of selected plugin"
  void set_plugin_list_master(const QColor &c);

  // == PluginSettings =======================================================
  // MO2 PluginSettings. KEYS ALREADY MATCH - this group is final and does not
  // move. plugins/disabled is MO2's Plugins/pluginBlacklist: both are the set
  // of plugins the user has turned off.
  //
  // plugins/settings/<basename>/<key> is deliberately NOT renamed to MO2's
  // flatter plugins/<basename>/<key>. It is the only move on the list that
  // crosses the ui -> engine boundary (two sites in
  // plugin_settings_registry.cpp), its basenames are open-ended so it needs
  // per-plugin enumeration the fixed table cannot express, and a partial
  // migration there is the worst case of all - engine and ui would disagree
  // about the key space and every plugin option would read as empty.
  QStringList disabled_plugins() const;  // key: plugins/disabled
  void set_disabled_plugins(const QStringList &names);
  bool plugin_enabled(const QString &name) const;
  void set_plugin_enabled(const QString &name, bool enabled);
  // Plugin-declared options (register_settings), persisted per plugin as
  // plain key:value pairs under plugins/settings/<basename>/<key>.
  QString plugin_setting(const QString &basename, const QString &key,
                         const QString &default_value) const;
  void set_plugin_setting(const QString &basename, const QString &key,
                          const QString &value);

  // == PathSettings =========================================================
  // MO2 PathSettings. KEYS ALREADY MATCH - this group is final and does not
  // move.
  //
  // recent_dirs keeps its two parallel QStringLists rather than becoming an
  // array or a map: IniFormat has no map encoder, which is why (settings.cpp).
  // GMM can configure only the instances root; MO2 has six configurable
  // directories. That is a feature gap, not a naming one.
  //
  // Instances root dir override (empty = default XDG location).
  QString instances_dir() const;
  void set_instances_dir(const QString &dir);

  // Last-used directory per named file dialog (MO2 PathSettings::recent,
  // settingsdialogpaths.cpp's FileDialogMemory). id -> directory.
  [[nodiscard]] QMap<QString, QString> recent_dirs() const;
  void set_recent_dirs(const QMap<QString, QString> &dirs);

  // == NetworkSettings ======================================================
  // MO2 NetworkSettings. KEYS ALREADY MATCH - this group is final and does not
  // move. proxy_host/proxy_port are GMM extras; MO2 uses the system proxy.
  bool offline_mode() const;
  void set_offline_mode(bool on);
  bool use_proxy() const;
  void set_use_proxy(bool on);
  QString proxy_host() const;
  void set_proxy_host(const QString &host);
  int proxy_port() const;
  void set_proxy_port(int port);
  bool use_custom_browser() const;
  void set_use_custom_browser(bool on);
  QString custom_browser_command() const;
  void set_custom_browser_command(const QString &cmd);

  // == NexusSettings + SteamSettings, merged into one SourceSettings =========
  // MO2 splits Nexus and Steam into two objects only because each has bespoke
  // fields and MO2 has no abstraction to hang them from. GMM has one
  // (engine::Source), so GMM gets ONE source object plus a provider-keyed
  // sub-namespace for the genuinely per-provider bits. This is the one place
  // the namespacing deliberately does NOT mirror MO2 1:1, and the reason is
  // precisely the abstraction the vendor-to-capability rule protects: none of
  // these keys names a vendor. Keys still live in nexus/, workshop/ and nxm/;
  // all three move to sources/ when this group is namespaced.
  //
  // endorsement_integration and tracked_integration are
  // already capability-named. They are Nexus features today, so they keep
  // their names as other sources grow them - unlike the sibling accessors,
  // which are named after the vendor that happened to ship first.
  bool endorsement_integration() const;
  void set_endorsement_integration(bool on);
  bool tracked_integration() const;
  void set_tracked_integration(bool on);
  // Queue Nexus downloads one-at-a-time. Free Regular/Supporter accounts are
  // throttled to ~1.5MB/s, so parallel transfers don't help them; only
  // Premium lifts the cap. Defaults to ON (multithreading OFF by default).
  // On login the tier-derived default is applied ONLY while the user has
  // never set the value explicitly - a manual choice survives later logins.
  bool nexus_queue_downloads() const;  // key: nexus/queue_downloads
  void set_nexus_queue_downloads(bool on);
  bool nexus_queue_downloads_set() const;  // user explicitly chose a value
  void remove_nexus_queue_downloads();     // back to tier-derived default
  // Client-side cooldown. Keyed by PROVIDER, not by a vendor, when it moves:
  // Steam is the only source with a cooldown today, LoversLab has none and
  // Nexus has a server-side budget, so keying it by provider means a second
  // source with a cooldown needs no new group.
  int workshop_rate_limit_per_hour() const;  // key: workshop/rate_limit_per_hour
  void set_workshop_rate_limit_per_hour(int n);
  // "Has this download link been handed to a handler already?" NXM is one
  // protocol among several (engine::ProtocolHandler has Nxm, Gmm and Modl), so
  // the concept is a download-link handler, not a vendor.
  QString nxm_handler_check() const;  // key: nxm/handler_check
  void set_nxm_handler_check(const QString &value);

  // == InterfaceSettings ====================================================
  // MO2 InterfaceSettings, the biggest class it has: everything the user
  // sees. Keys still live in interface/, downloads/, close/, appearance/,
  // preview/ and at the top level (language/style/theme); all of those move
  // under interface/ when this group is namespaced.
  // Grey the interface out during operations that lock it (MO2
  // InterfaceSettings::lockGUI, settings.cpp:2168). Key: interface/lock_gui.
  // Defaults ON, MO2's default.
  bool lock_gui() const;
  void set_lock_gui(bool on);
  QString language() const;
  void set_language(const QString &tag);
  bool smooth_scrolling() const;
  void set_smooth_scrolling(bool on);
  bool show_download_notifications() const;
  void set_show_download_notifications(bool on);
  bool hide_installed_downloads() const;  // key: downloads/hide_installed
  void set_hide_installed_downloads(bool on);
  // Double-click opens previews (MO2 doubleClicksOpenPreviews): the persisted
  // default for file double-clicks in the file trees. ON = plain double-click
  // previews a file built-in when a preview handler exists (else OS-open).
  // OFF (default) = plain double-click OS-opens. Alt inverts the setting for
  // that one click; Ctrl+double-click reveals the containing folder in
  // Explorer (the same convention the mod list uses).
  // Governs file open-vs-preview only - not mod open/install behaviors.
  bool
  double_clicks_open_previews() const;  // key: interface/double_clicks_open_previews
  void set_double_clicks_open_previews(bool on);
  bool compact_downloads() const;  // key: downloads/compact
  void set_compact_downloads(bool on);
  bool display_foreign() const;
  void set_display_foreign(bool on);
  bool save_filters() const;
  void set_save_filters(bool on);
  bool auto_collapse_on_hover() const;
  void set_auto_collapse_on_hover(bool on);
  bool collapsible_separators_per_profile() const;
  void set_collapsible_separators_per_profile(bool on);
  bool collapsible_separators_highlight_from() const;
  void set_collapsible_separators_highlight_from(bool on);
  bool collapsible_separators_highlight_to() const;
  void set_collapsible_separators_highlight_to(bool on);
  bool collapsible_separators_asc() const;
  void set_collapsible_separators_asc(bool on);
  bool collapsible_separators_dsc() const;
  void set_collapsible_separators_dsc(bool on);
  bool collapsible_separators_icons_conflicts() const;
  void set_collapsible_separators_icons_conflicts(bool on);
  bool collapsible_separators_icons_flags() const;
  void set_collapsible_separators_icons_flags(bool on);
  bool collapsible_separators_icons_content() const;
  void set_collapsible_separators_icons_content(bool on);
  bool collapsible_separators_icons_version() const;
  void set_collapsible_separators_icons_version(bool on);
  // The collapsible_separators_icons_* keys are keyed by NAME here, where MO2
  // keys them by COLUMN INDEX (collapsible_separators_icons_0/1/2/...). Keep
  // GMM's names: an index-keyed flag silently changes meaning when a column is
  // reordered, a named one does not. Deliberate divergence from MO2, and the
  // right one.
  //
  // Check the mod update database after a mod finishes installing. This is a
  // MOD update check, not an app update: MO2's equivalent
  // (InterfaceSettings::checkUpdateAfterInstallation) is consumed by
  // MainWindow::modInstalled, which asks the mod update DB about the mod that
  // was just installed. The key was interface/check_update_after_install,
  // which read as an app update and would have been wired to the wrong
  // feature; the old key is still read (see the .cpp) so an existing stored
  // choice survives the rename. Default ON.
  //
  // interface/check_update_after_install is RETIRED and stays retired. Its
  // read-fallback is deliberate and tested (settings_updater_test.cpp asserts
  // the old key is never written), so it is deliberately absent from the
  // migration table: folding it in would reintroduce a key the project chose
  // to retire.
  bool check_update_after_mod_install() const;
  void set_check_update_after_mod_install(bool on);
  bool hide_api_counter() const;
  void set_hide_api_counter(bool on);
  bool show_change_game_confirmation() const;
  void set_show_change_game_confirmation(bool on);
  // Restart the app on every instance switch instead of live-switching the
  // running window. Defaults to ON: a live switch reuses every controller,
  // model and in-flight worker, which is the root of the cross-instance
  // stale-state bug class (stale mod lists, plugins DB, pending changes and
  // orphaned downloads). A restart gives each instance a fresh window.
  bool restart_on_instance_switch() const;  // key: interface/restart_on_instance_switch
  void set_restart_on_instance_switch(bool on);
  // Full UI tab mode: Settings/Pipeline open as in-window tabs instead of
  // popup dialogs. Defaults to OFF (popup behavior, identical to the
  // pre-tab layout).
  bool full_ui_mode() const;  // key: interface/full_ui_mode
  void set_full_ui_mode(bool on);
  // Settings > Mod List > "Remember filter settings" (default on). The set of
  // category ids the category filter panel had ticked when it was last
  // changed, as a CSV. Empty = no remembered filter.
  QStringList modlist_filter_categories() const;  // key: interface/filter_categories
  void set_modlist_filter_categories(const QStringList &ids);
  // The same setting's other two halves. MO2's FilterList::saveState
  // (filterlist.cpp:245-251) persists the ticked special criteria AND the
  // And/Or mode alongside the criteria tree, so "remember my filters"
  // remembers the whole flyout and not just its categories.
  QStringList modlist_filter_specials() const;  // key: interface/filter_specials
  void set_modlist_filter_specials(const QStringList &names);
  bool modlist_filter_any_mode() const;  // key: interface/filter_any_mode
  void set_modlist_filter_any_mode(bool on);

  // Ask for confirmation before closing while downloads are still running
  // (the close cancels them). "Don't Ask" persists this as false.
  bool confirm_close_with_downloads() const;  // key: close/confirm_downloads
  void set_confirm_close_with_downloads(bool on);

  // Hide to the system tray instead of quitting when the window is closed.
  // Defaults OFF, so the titlebar X quits exactly as it does everywhere else.
  // Only takes effect on a system that actually has a tray; without one the
  // close quits regardless, so the app can never end up unreachable.
  bool minimize_to_tray() const;  // key: close/minimize_to_tray
  void set_minimize_to_tray(bool on);

  QString theme() const;  // GMM QSS theme name ("default")
  void set_theme(const QString &name);
  void clear_theme();     // no QSS theme selected
  QString style() const;  // Qt built-in style ("Fusion", ...)
  void set_style(const QString &name);
  void clear_style();  // no Qt built-in style selected

  // "default" (theme icons first, then system), "system" (ignore theme/pack
  // icons), or a bundled pack name from resources/icons/packs/ (e.g. "MO2").
  QString icon_pack() const;
  void set_icon_pack(const QString &name);

  // Center the label on separator rows (Settings > Theme > Design). MO2
  // centers separator text by default; off restores left-alignment.
  bool center_separator_text() const;
  void set_center_separator_text(bool on);

  // Transparency checkerboard style for image previews (PreviewWindow and
  // the Mod Info image viewer): 0=off, 1=light, 2=medium, 3=dark.
  int checkerboard_style() const;  // key: preview/checkerboard_style
  void set_checkerboard_style(int style);

  // FOMOD install wizard behavior. Restore-on-reinstall defaults to on so the
  // wizard re-applies the previously persisted choices (MO2 behavior);
  // image previews default to on.
  bool always_restore_fomod_choices() const;  // key: fomod/always_restore_choices
  void set_always_restore_fomod_choices(bool on);
  bool show_fomod_images() const;  // key: fomod/show_images
  void set_show_fomod_images(bool on);

  // == DiagnosticsSettings ==================================================
  // MO2 DiagnosticsSettings. KEYS ALREADY MATCH - this group is final and does
  // not move. MO2 additionally has crash_dumps_type, loot_log_level and
  // spawn_delay; those are missing features, not naming ones.
  QString log_level() const;  // "debug"/"info"/"warn"/"error"
  void set_log_level(const QString &level);
  int max_core_dumps() const;
  void set_max_core_dumps(int n);

  // == Settings (root) ======================================================
  // MO2's root Settings class. Every key here has no better home, which is
  // exactly what MO2's root object is for; MO2 flattens it to the ini
  // top level, GMM keeps it under settings/. Keys still live in general/,
  // extraction/, performance/, profile/, workarounds/ and archive/; all move
  // under settings/ when this group is namespaced.
  //
  // Allow a periodic background app-update check (at most once per 24h).
  // Default OFF: a check on every launch is an unauthenticated request to a
  // third party on every start, which discloses the user's IP, the app
  // version and the fact they run this app. The label in the settings panel
  // names this cadence, not a startup check.
  bool check_for_updates() const;  // key: general/check_for_updates
  void set_check_for_updates(bool on);
  // When the last successful check ran, so the 24h timer means something and
  // the panel can say when it last looked. Invalid = never checked.
  QDateTime last_update_check() const;  // key: general/last_update_check
  void set_last_update_check(const QDateTime &when);
  // Prerelease channel selection. There is no channel concept on the release
  // feed, so the panel keeps this control disabled with a tooltip naming the
  // prerequisite. The value is still stored so nothing is lost. Deliberately
  // disabled, NOT dead-by-accident: settings_updater_test.cpp asserts the
  // disclosure. Move it; do not wire it.
  bool use_prereleases() const;
  void set_use_prereleases(bool on);

  // Lower the CPU priority of archive extraction so large mod archives don't
  // max out the system while installing. Defaults to ON (most users want
  // this); read on every install, never cached at startup.
  bool extraction_low_priority() const;  // key: extraction/low_priority
  void set_extraction_low_priority(bool on);

  // Whether the "Keep Backup" checkbox on the Mod Exists dialog starts ticked.
  // Defaults to OFF, matching MO2's General/backup_install default
  // (references/modorganizer/src/settings.cpp:412-420). Read on every
  // collision and written back from the answer the user gave, so the dialog
  // opens on what they last chose rather than on a fixed value.
  bool keep_backup_on_install() const;  // key: extraction/backup_install
  void set_keep_backup_on_install(bool on);

  // Enable multi-core processing for per-item CPU work (mod scanning, etc.).
  // Live via ui/settings/settings_content_widget.cpp ->
  // engine::parallel::set_enabled(). Defaults to ON; off forces
  // engine::parallel::for_each to fall back to the sequential loop for debugging /
  // single-core-equivalent behavior. Note: deploy stays always-parallel regardless of
  // this toggle.
  bool performance_multi_core() const;  // key: performance/enable_multicore
  void set_performance_multi_core(bool on);

  bool local_saves() const;
  void set_local_saves(bool on);
  bool local_inis() const;
  void set_local_inis(bool on);
  bool archive_invalidation() const;
  void set_archive_invalidation(bool on);

  QString executables_blacklist() const;
  void set_executables_blacklist(const QString &value);
  QStringList skip_file_suffixes() const;
  void set_skip_file_suffixes(const QStringList &values);
  QStringList skip_directories() const;
  void set_skip_directories(const QStringList &values);
  bool experimental_archive_parsing() const;
  void set_experimental_archive_parsing(bool on);
  // Push the two Workarounds lists into the engine. The engine is Qt-free and
  // cannot read QSettings, so the values are handed over here: called once at
  // startup and again whenever the tab writes, so a scan already running picks
  // a new value up without a restart.
  void apply_workarounds();
  // Post-launch overwrite-capture delay (launch_controller.cpp). Root is the
  // right bucket: it is not interface, not diagnostics, not paths.
  int overlay_capture_delay_ms() const;
  void set_overlay_capture_delay_ms(int ms);

private:
  // Runs the one-shot settings migration before instance() hands this object
  // out, i.e. before the first read in the process (app/core.cpp's
  // max_core_dumps()). The table it applies is still empty; the gate it runs is
  // the thing that has to exist before any key moves.
  Settings();
  // The format is now an explicit argument instead of being implied by the
  // (organization, application) constructor. QSettings::defaultFormat() is
  // QSettings::NativeFormat unless someone called setDefaultFormat(), and no
  // production code does, so this resolves to exactly the same store the
  // org/app ctor used (same file, same key space, same fallback chain).
  // Tests get the sandbox instead: the shared Catch2 runner calls
  // setDefaultFormat(IniFormat) + setPath(IniFormat, UserScope, tmpdir)
  // before QApplication exists (Workspace-2zm). IniFormat + setPath is
  // honoured on EVERY platform, so no test can reach the macOS
  // CFPreferences plist, the Windows registry, or the user's ~/.config.
  QSettings settings_{QSettings::defaultFormat(), QSettings::UserScope,
                      "GameModManager", "GameModManager"};
};
