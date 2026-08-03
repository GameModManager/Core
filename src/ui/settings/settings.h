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

private:
    Settings() = default;
    QSettings settings_{"GameModManager", "GameModManager"};
};
