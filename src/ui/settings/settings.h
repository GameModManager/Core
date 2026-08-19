#pragma once

#include <QColor>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <optional>

// App-level settings facade. All QSettings access goes through this class so
// keys stay centralized and typed, mirroring MO2's Settings singleton.
// Writes are immediate (per-control on change); keys keep their historical
// names where they already existed to avoid a migration.
class Settings {
public:
    static Settings& instance();

    // interface ------------------------------------------------------------
    QString language() const;
    void set_language(const QString& tag);
    bool smooth_scrolling() const;
    void set_smooth_scrolling(bool on);
    bool show_download_notifications() const;
    void set_show_download_notifications(bool on);
    bool hide_installed_downloads() const;   // key: downloads/hide_installed
    void set_hide_installed_downloads(bool on);
    bool compact_downloads() const;          // key: downloads/compact
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
    bool check_update_after_install() const;
    void set_check_update_after_install(bool on);
    bool hide_api_counter() const;
    void set_hide_api_counter(bool on);
    bool show_change_game_confirmation() const;
    void set_show_change_game_confirmation(bool on);
    // Full UI tab mode: Settings/Pipeline open as in-window tabs instead of
    // popup dialogs. Defaults to OFF (popup behavior, identical to the
    // pre-tab layout).
    bool full_ui_mode() const;               // key: interface/full_ui_mode
    void set_full_ui_mode(bool on);

    // Ask for confirmation before closing while downloads are still running
    // (the close cancels them). "Don't Ask" persists this as false.
    bool confirm_close_with_downloads() const;   // key: close/confirm_downloads
    void set_confirm_close_with_downloads(bool on);

    // Default (startup) profile name for the current instance. Empty when no
    // default was chosen — the app then falls back to the first profile.
    QString default_profile() const;             // key: profiles/default
    void set_default_profile(const QString& name);

    // mod list columns --------------------------------------------------------
    // Per-instance set of mod-list columns the user has hidden (by column
    // name, stable across reordering). Missing key = first-run defaults apply.
    // The Name column is never stored here (it cannot be hidden).
    QStringList modlist_hidden_columns(const QString& instance_name) const;
    void set_modlist_hidden_columns(const QString& instance_name,
                                    const QStringList& hidden);
    // Per-instance "nested mod list" toggle: allows dragging mods onto mods and
    // separators onto separators to build a visual nesting (indented, foldable
    // children) without changing load order / priorities. Defaults to off.
    bool modlist_nested(const QString& instance_name) const;
    void set_modlist_nested(const QString& instance_name, bool on);

    // general ---------------------------------------------------------------
    bool check_for_updates() const;
    void set_check_for_updates(bool on);
    bool use_prereleases() const;
    void set_use_prereleases(bool on);

    // profile defaults -------------------------------------------------------
    bool local_saves() const;
    void set_local_saves(bool on);
    bool local_inis() const;
    void set_local_inis(bool on);
    bool archive_invalidation() const;
    void set_archive_invalidation(bool on);

    // theme ------------------------------------------------------------------
    QString theme() const;                 // GMM QSS theme name ("default")
    void set_theme(const QString& name);
    void clear_theme();                    // no QSS theme selected
    QString style() const;                 // Qt built-in style ("Fusion", ...)
    void set_style(const QString& name);
    void clear_style();                    // no Qt built-in style selected

    // icon pack ------------------------------------------------------------
    // "default" (theme icons first, then system), "system" (ignore theme/pack
    // icons), or a bundled pack name from resources/icons/packs/ (e.g. "MO2").
    QString icon_pack() const;
    void set_icon_pack(const QString& name);

    // geometry ---------------------------------------------------------------
    bool center_dialogs() const;
    void set_center_dialogs(bool on);
    void reset_dialog_geometry();

    // paths ------------------------------------------------------------------
    // Instances root dir override (empty = default XDG location).
    QString instances_dir() const;
    void set_instances_dir(const QString& dir);

    // network ----------------------------------------------------------------
    bool offline_mode() const;
    void set_offline_mode(bool on);
    bool use_proxy() const;
    void set_use_proxy(bool on);
    QString proxy_host() const;
    void set_proxy_host(const QString& host);
    int proxy_port() const;
    void set_proxy_port(int port);
    bool use_custom_browser() const;
    void set_use_custom_browser(bool on);
    QString custom_browser_command() const;
    void set_custom_browser_command(const QString& cmd);

    // nexus source -----------------------------------------------------------
    bool endorsement_integration() const;
    void set_endorsement_integration(bool on);
    bool tracked_integration() const;
    void set_tracked_integration(bool on);
    bool category_mappings() const;
    void set_category_mappings(bool on);
    // Queue Nexus downloads one-at-a-time. Free Regular/Supporter accounts are
    // throttled to ~1.5MB/s, so parallel transfers don't help them; only
    // Premium lifts the cap. Defaults to ON (multithreading OFF by default).
    // On login the tier-derived default is applied ONLY while the user has
    // never set the value explicitly — a manual choice survives later logins.
    bool nexus_queue_downloads() const;           // key: nexus/queue_downloads
    void set_nexus_queue_downloads(bool on);
    bool nexus_queue_downloads_set() const;       // user explicitly chose a value
    void remove_nexus_queue_downloads();          // back to tier-derived default

    // workshop source ---------------------------------------------------------
    int workshop_rate_limit_per_hour() const;   // key: workshop/rate_limit_per_hour
    void set_workshop_rate_limit_per_hour(int n);

    // workarounds ---------------------------------------------------------------
    QString executables_blacklist() const;
    void set_executables_blacklist(const QString& value);
    QStringList skip_file_suffixes() const;
    void set_skip_file_suffixes(const QStringList& values);
    QStringList skip_directories() const;
    void set_skip_directories(const QStringList& values);
    bool force_enable_core_files() const;
    void set_force_enable_core_files(bool on);
    bool experimental_archive_parsing() const;
    void set_experimental_archive_parsing(bool on);
    int overlay_capture_delay_ms() const;
    void set_overlay_capture_delay_ms(int ms);

    // diagnostics ---------------------------------------------------------------
    QString log_level() const;              // "debug"/"info"/"warn"/"error"
    void set_log_level(const QString& level);
    int max_core_dumps() const;
    void set_max_core_dumps(int n);
    QString core_dump_type() const;         // "text"/"full"
    void set_core_dump_type(const QString& type);

    // colors -------------------------------------------------------------------
    bool color_separator_scrollbar() const;
    void set_color_separator_scrollbar(bool on);
    // Center the label on separator rows (Settings > Theme > Design). MO2
    // centers separator text by default; off restores left-alignment.
    bool center_separator_text() const;
    void set_center_separator_text(bool on);
    // Remembered color for the next separator (MO2 previousSeparatorColor).
    // Hidden setting - never shown in the settings dialog.
    std::optional<QColor> previous_separator_color() const;
    void set_previous_separator_color(const QColor& c);
    void remove_previous_separator_color();
    // MO2 mod-list conflict colors; defaults match MO2's colortable.cpp.
    QColor modlist_overwritten_loose() const;    // "Is overwritten (loose files)"
    void set_modlist_overwritten_loose(const QColor& c);
    QColor modlist_overwriting_loose() const;    // "Is overwriting (loose files)"
    void set_modlist_overwriting_loose(const QColor& c);
    QColor modlist_overwritten_archive() const;  // "Is overwritten (archives)"
    void set_modlist_overwritten_archive(const QColor& c);
    QColor modlist_overwriting_archive() const;  // "Is overwriting (archives)"
    void set_modlist_overwriting_archive(const QColor& c);
    QColor modlist_contains_file() const;        // "Mod contains selected file"
    void set_modlist_contains_file(const QColor& c);
    QColor plugin_list_contained() const;        // "Plugin is contained in selected mod"
    void set_plugin_list_contained(const QColor& c);
    QColor plugin_list_master() const;           // "Plugin is master of selected plugin"
    void set_plugin_list_master(const QColor& c);

    // plugins -------------------------------------------------------------------
    QStringList disabled_plugins() const;        // key: plugins/disabled
    void set_disabled_plugins(const QStringList& names);
    bool plugin_enabled(const QString& name) const;
    void set_plugin_enabled(const QString& name, bool enabled);

    // Plugin-declared options (register_settings), persisted per plugin as
    // plain key:value pairs under plugins/settings/<basename>/<key>.
    QString plugin_setting(const QString& basename, const QString& key,
                           const QString& default_value) const;
    void set_plugin_setting(const QString& basename, const QString& key,
                            const QString& value);

    // nxm -----------------------------------------------------------------------
    QString nxm_handler_check() const;      // key: nxm/handler_check
    void set_nxm_handler_check(const QString& value);

    // fomod ---------------------------------------------------------------------
    // FOMOD install wizard behavior. Restore-on-reinstall defaults to on so the
    // wizard re-applies the previously persisted choices (MO2 behavior);
    // image previews default to on.
    bool always_restore_fomod_choices() const;   // key: fomod/always_restore_choices
    void set_always_restore_fomod_choices(bool on);
    bool show_fomod_images() const;              // key: fomod/show_images
    void set_show_fomod_images(bool on);
    // Last wizard geometry + splitter states (empty until the first install).
    QByteArray fomod_window_geometry() const;    // key: fomod/window_geometry
    void set_fomod_window_geometry(const QByteArray& g);
    QByteArray fomod_center_split() const;       // key: fomod/center_split
    void set_fomod_center_split(const QByteArray& s);
    QByteArray fomod_left_split() const;         // key: fomod/left_split
    void set_fomod_left_split(const QByteArray& s);

    // mod info -----------------------------------------------------------------
    // Mod Info dialog geometry + last active tab (restored on next open).
    QByteArray modinfo_window_geometry() const;  // key: modinfo/window_geometry
    void set_modinfo_window_geometry(const QByteArray& g);
    int modinfo_last_tab() const;                // key: modinfo/last_tab
    void set_modinfo_last_tab(int index);

    // ListDialog (generic choice picker) geometry — restored on next open.
    QByteArray listdialog_window_geometry() const;  // key: listdialog/window_geometry
    void set_listdialog_window_geometry(const QByteArray& g);

private:
    Settings() = default;
    QSettings settings_{"GameModManager", "GameModManager"};
};
