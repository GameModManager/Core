#include "ui/settings/settings.h"

#include "engine/util/fs_utils.h"
#include "ui/widgets/mod_list_model.h"

#include <algorithm>

Settings &Settings::instance() {
  static Settings s;
  return s;
}

// interface ----------------------------------------------------------------

QString Settings::language() const {
  return settings_.value("language", "en_US").toString();
}

void Settings::set_language(const QString &tag) {
  settings_.setValue("language", tag);
}

bool Settings::smooth_scrolling() const {
  return settings_.value("interface/smooth_scrolling", true).toBool();
}

void Settings::set_smooth_scrolling(bool on) {
  settings_.setValue("interface/smooth_scrolling", on);
}

bool Settings::show_download_notifications() const {
  return settings_.value("interface/show_download_notifications", true).toBool();
}

void Settings::set_show_download_notifications(bool on) {
  settings_.setValue("interface/show_download_notifications", on);
}

bool Settings::hide_installed_downloads() const {
  return settings_.value("downloads/hide_installed", false).toBool();
}

void Settings::set_hide_installed_downloads(bool on) {
  settings_.setValue("downloads/hide_installed", on);
}

bool Settings::compact_downloads() const {
  return settings_.value("downloads/compact", true).toBool();
}

void Settings::set_compact_downloads(bool on) {
  settings_.setValue("downloads/compact", on);
}

bool Settings::double_clicks_open_previews() const {
  return settings_.value("interface/double_clicks_open_previews", false).toBool();
}

void Settings::set_double_clicks_open_previews(bool on) {
  settings_.setValue("interface/double_clicks_open_previews", on);
}

bool Settings::display_foreign() const {
  return settings_.value("interface/display_foreign", true).toBool();
}

void Settings::set_display_foreign(bool on) {
  settings_.setValue("interface/display_foreign", on);
}

bool Settings::save_filters() const {
  return settings_.value("interface/save_filters", true).toBool();
}

void Settings::set_save_filters(bool on) {
  settings_.setValue("interface/save_filters", on);
}

bool Settings::auto_collapse_on_hover() const {
  return settings_.value("interface/auto_collapse_on_hover", false).toBool();
}

void Settings::set_auto_collapse_on_hover(bool on) {
  settings_.setValue("interface/auto_collapse_on_hover", on);
}

bool Settings::collapsible_separators_per_profile() const {
  return settings_.value("interface/collapsible_separators_per_profile", true).toBool();
}

void Settings::set_collapsible_separators_per_profile(bool on) {
  settings_.setValue("interface/collapsible_separators_per_profile", on);
}

bool Settings::collapsible_separators_highlight_from() const {
  return settings_.value("interface/collapsible_separators_highlight_from", false)
      .toBool();
}

void Settings::set_collapsible_separators_highlight_from(bool on) {
  settings_.setValue("interface/collapsible_separators_highlight_from", on);
}

// On by default: the band-derived separator highlight is what the mod list has
// always drawn, so "off" would take it away from anyone who never opened
// Settings. Storing false is what turns it off.
bool Settings::collapsible_separators_highlight_to() const {
  return settings_.value("interface/collapsible_separators_highlight_to", true)
      .toBool();
}

void Settings::set_collapsible_separators_highlight_to(bool on) {
  settings_.setValue("interface/collapsible_separators_highlight_to", on);
}

bool Settings::collapsible_separators_asc() const {
  return settings_.value("interface/collapsible_separators_asc", true).toBool();
}

void Settings::set_collapsible_separators_asc(bool on) {
  settings_.setValue("interface/collapsible_separators_asc", on);
}

bool Settings::collapsible_separators_dsc() const {
  return settings_.value("interface/collapsible_separators_dsc", false).toBool();
}

void Settings::set_collapsible_separators_dsc(bool on) {
  settings_.setValue("interface/collapsible_separators_dsc", on);
}

bool Settings::collapsible_separators_icons_conflicts() const {
  return settings_.value("interface/collapsible_separators_icons_conflicts", true)
      .toBool();
}

void Settings::set_collapsible_separators_icons_conflicts(bool on) {
  settings_.setValue("interface/collapsible_separators_icons_conflicts", on);
}

bool Settings::collapsible_separators_icons_flags() const {
  return settings_.value("interface/collapsible_separators_icons_flags", true).toBool();
}

void Settings::set_collapsible_separators_icons_flags(bool on) {
  settings_.setValue("interface/collapsible_separators_icons_flags", on);
}

bool Settings::collapsible_separators_icons_content() const {
  return settings_.value("interface/collapsible_separators_icons_content", true)
      .toBool();
}

void Settings::set_collapsible_separators_icons_content(bool on) {
  settings_.setValue("interface/collapsible_separators_icons_content", on);
}

bool Settings::collapsible_separators_icons_version() const {
  return settings_.value("interface/collapsible_separators_icons_version", true)
      .toBool();
}

void Settings::set_collapsible_separators_icons_version(bool on) {
  settings_.setValue("interface/collapsible_separators_icons_version", on);
}

// The pre-rename key for the mod-update check. It read as an app-update
// setting, which it never was. Read as a fallback rather than rewritten: a
// stored value under the old name is honoured until the user touches the
// checkbox (which writes the new key, which then wins), and nothing is ever
// written during a read, so the migration cannot damage the config.
static constexpr const char *kCheckUpdateAfterInstallKey =
    "interface/check_update_after_install";
static constexpr const char *kCheckUpdateAfterModInstallKey =
    "interface/check_update_after_mod_install";

bool Settings::check_update_after_mod_install() const {
  if (settings_.contains(kCheckUpdateAfterModInstallKey))
    return settings_.value(kCheckUpdateAfterModInstallKey, true).toBool();
  if (settings_.contains(kCheckUpdateAfterInstallKey))
    return settings_.value(kCheckUpdateAfterInstallKey, true).toBool();
  return true;
}

void Settings::set_check_update_after_mod_install(bool on) {
  settings_.setValue(kCheckUpdateAfterModInstallKey, on);
}

bool Settings::hide_api_counter() const {
  return settings_.value("interface/hide_api_counter", false).toBool();
}

void Settings::set_hide_api_counter(bool on) {
  settings_.setValue("interface/hide_api_counter", on);
}

bool Settings::show_change_game_confirmation() const {
  return settings_.value("interface/show_change_game_confirmation", true).toBool();
}

void Settings::set_show_change_game_confirmation(bool on) {
  settings_.setValue("interface/show_change_game_confirmation", on);
}

bool Settings::full_ui_mode() const {
  return settings_.value("interface/full_ui_mode", false).toBool();
}

void Settings::set_full_ui_mode(bool on) {
  settings_.setValue("interface/full_ui_mode", on);
}

bool Settings::confirm_close_with_downloads() const {
  return settings_.value("close/confirm_downloads", true).toBool();
}

void Settings::set_confirm_close_with_downloads(bool on) {
  settings_.setValue("close/confirm_downloads", on);
}

bool Settings::minimize_to_tray() const {
  return settings_.value("close/minimize_to_tray", false).toBool();
}

void Settings::set_minimize_to_tray(bool on) {
  settings_.setValue("close/minimize_to_tray", on);
}

QString Settings::default_profile() const {
  return settings_.value("profiles/default").toString();
}

void Settings::set_default_profile(const QString &name) {
  settings_.setValue("profiles/default", name);
}

// mod list columns ---------------------------------------------------------

QStringList Settings::modlist_hidden_columns(const QString &instance_name) const {
  return settings_.value("modlist/columns/" + instance_name).toStringList();
}

void Settings::set_modlist_hidden_columns(const QString &instance_name,
                                          const QStringList &hidden) {
  settings_.setValue("modlist/columns/" + instance_name, hidden);
}

void Settings::ensure_modlist_column_defaults(const QString &instance_name) {
  // contains(), not emptiness: a user who deliberately unhides every column
  // persists an empty list under an existing key and must keep it.
  if (!settings_.contains("modlist/columns/" + instance_name))
    set_modlist_hidden_columns(instance_name,
                               ui::ModList::default_hidden_column_names());
}

bool Settings::modlist_nested(const QString &instance_name) const {
  return settings_.value("modlist/nested/" + instance_name, false).toBool();
}

void Settings::set_modlist_nested(const QString &instance_name, bool on) {
  settings_.setValue("modlist/nested/" + instance_name, on);
}

QStringList Settings::modlist_filter_categories() const {
  return settings_.value("interface/filter_categories").toStringList();
}

void Settings::set_modlist_filter_categories(const QStringList &ids) {
  settings_.setValue("interface/filter_categories", ids);
}

// general -----------------------------------------------------------------

bool Settings::check_for_updates() const {
  return settings_.value("general/check_for_updates", false).toBool();
}

void Settings::set_check_for_updates(bool on) {
  settings_.setValue("general/check_for_updates", on);
}

QDateTime Settings::last_update_check() const {
  return settings_.value("general/last_update_check").toDateTime();
}

void Settings::set_last_update_check(const QDateTime &when) {
  settings_.setValue("general/last_update_check", when);
}

bool Settings::use_prereleases() const {
  return settings_.value("general/use_prereleases", false).toBool();
}

void Settings::set_use_prereleases(bool on) {
  settings_.setValue("general/use_prereleases", on);
}

bool Settings::extraction_low_priority() const {
  return settings_.value("extraction/low_priority", true).toBool();
}

void Settings::set_extraction_low_priority(bool on) {
  settings_.setValue("extraction/low_priority", on);
}

bool Settings::keep_backup_on_install() const {
  return settings_.value("extraction/backup_install", false).toBool();
}

void Settings::set_keep_backup_on_install(bool on) {
  settings_.setValue("extraction/backup_install", on);
}

bool Settings::performance_multi_core() const {
  return settings_.value("performance/enable_multicore", true).toBool();
}

void Settings::set_performance_multi_core(bool on) {
  settings_.setValue("performance/enable_multicore", on);
}

// profile defaults ----------------------------------------------------------

bool Settings::local_saves() const {
  return settings_.value("profile/local_saves", false).toBool();
}

void Settings::set_local_saves(bool on) {
  settings_.setValue("profile/local_saves", on);
}

bool Settings::local_inis() const {
  return settings_.value("profile/local_inis", false).toBool();
}

void Settings::set_local_inis(bool on) {
  settings_.setValue("profile/local_inis", on);
}

bool Settings::archive_invalidation() const {
  return settings_.value("profile/archive_invalidation", false).toBool();
}

void Settings::set_archive_invalidation(bool on) {
  settings_.setValue("profile/archive_invalidation", on);
}

// theme ---------------------------------------------------------------------

QString Settings::theme() const {
  return settings_.value("theme", "default").toString();
}

void Settings::set_theme(const QString &name) {
  settings_.setValue("theme", name);
}

QString Settings::style() const {
  return settings_.value("style").toString();
}

void Settings::set_style(const QString &name) {
  settings_.setValue("style", name);
}

void Settings::clear_theme() {
  settings_.remove("theme");
}

void Settings::clear_style() {
  settings_.remove("style");
}

// icon pack ------------------------------------------------------------------

QString Settings::icon_pack() const {
  return settings_.value("appearance/icon_pack", "default").toString();
}

void Settings::set_icon_pack(const QString &name) {
  settings_.setValue("appearance/icon_pack", name);
}

// geometry -------------------------------------------------------------------

bool Settings::center_dialogs() const {
  return settings_.value("geometry/center_dialogs", false).toBool();
}

void Settings::set_center_dialogs(bool on) {
  settings_.setValue("geometry/center_dialogs", on);
}

void Settings::reset_dialog_geometry() {
  // Each dialog keeps its geometry under its OWN group (fomod/, modinfo/,
  // listdialog/, saveinfo/), not under "geometry", so remove() on that group
  // cleared none of them: the only key in it is center_dialogs, so the button
  // silently switched off "Center dialogs on screen" and every dialog came back
  // at the size and place the user had just reset. Clearing through the setters
  // keeps the key list in one place with the getters.
  set_fomod_window_geometry({});
  set_modinfo_window_geometry({});
  set_listdialog_window_geometry({});
  set_saveinfo_window_geometry({});
}

namespace {

QString dialog_choice_key(const QString &action, const QString &file) {
  if (file.isEmpty())
    return "dialog_choices/" + action;
  return "dialog_choices/" + action + "/" + file;
}

}  // namespace

std::optional<QMessageBox::StandardButton>
Settings::dialog_choice(const QString &action, const QString &file) const {
  const QString key = dialog_choice_key(action, file);
  if (!settings_.contains(key))
    return std::nullopt;
  return static_cast<QMessageBox::StandardButton>(settings_.value(key).toInt());
}

void Settings::set_dialog_choice(const QString &action, const QString &file,
                                 QMessageBox::StandardButton button) {
  settings_.setValue(dialog_choice_key(action, file), static_cast<int>(button));
}

void Settings::reset_dialog_choices() {
  settings_.beginGroup("dialog_choices");
  settings_.remove("");
  settings_.endGroup();
}

// paths -----------------------------------------------------------------------

QString Settings::instances_dir() const {
  return settings_.value("paths/instances").toString();
}

void Settings::set_instances_dir(const QString &dir) {
  settings_.setValue("paths/instances", dir);
}

QMap<QString, QString> Settings::recent_dirs() const {
  // Two parallel lists rather than a QVariantMap: the IniFormat codec the
  // tests run under has no map encoder, and QSettings stores a list as-is.
  QMap<QString, QString> dirs;
  const QStringList keys = settings_.value("paths/recent_keys").toStringList();
  const QStringList vals = settings_.value("paths/recent_dirs").toStringList();
  const int n            = qMin(keys.size(), vals.size());
  for (int i = 0; i < n; ++i)
    dirs.insert(keys.at(i), vals.at(i));
  return dirs;
}

void Settings::set_recent_dirs(const QMap<QString, QString> &dirs) {
  QStringList keys;
  QStringList vals;
  for (auto it = dirs.cbegin(); it != dirs.cend(); ++it) {
    keys.append(it.key());
    vals.append(it.value());
  }
  settings_.setValue("paths/recent_keys", keys);
  settings_.setValue("paths/recent_dirs", vals);
}

// network ---------------------------------------------------------------------

bool Settings::offline_mode() const {
  return settings_.value("network/offline_mode", false).toBool();
}

void Settings::set_offline_mode(bool on) {
  settings_.setValue("network/offline_mode", on);
}

bool Settings::use_proxy() const {
  return settings_.value("network/use_proxy", false).toBool();
}

void Settings::set_use_proxy(bool on) {
  settings_.setValue("network/use_proxy", on);
}

QString Settings::proxy_host() const {
  return settings_.value("network/proxy_host").toString();
}

void Settings::set_proxy_host(const QString &host) {
  settings_.setValue("network/proxy_host", host);
}

int Settings::proxy_port() const {
  return settings_.value("network/proxy_port", 8080).toInt();
}

void Settings::set_proxy_port(int port) {
  settings_.setValue("network/proxy_port", port);
}

bool Settings::use_custom_browser() const {
  return settings_.value("network/use_custom_browser", false).toBool();
}

void Settings::set_use_custom_browser(bool on) {
  settings_.setValue("network/use_custom_browser", on);
}

QString Settings::custom_browser_command() const {
  return settings_.value("network/custom_browser_command").toString();
}

void Settings::set_custom_browser_command(const QString &cmd) {
  settings_.setValue("network/custom_browser_command", cmd);
}

// nexus source ----------------------------------------------------------------

bool Settings::endorsement_integration() const {
  return settings_.value("nexus/endorsement_integration", true).toBool();
}

void Settings::set_endorsement_integration(bool on) {
  settings_.setValue("nexus/endorsement_integration", on);
}

bool Settings::tracked_integration() const {
  return settings_.value("nexus/tracked_integration", true).toBool();
}

void Settings::set_tracked_integration(bool on) {
  settings_.setValue("nexus/tracked_integration", on);
}

bool Settings::category_mappings() const {
  return settings_.value("nexus/category_mappings", false).toBool();
}

void Settings::set_category_mappings(bool on) {
  settings_.setValue("nexus/category_mappings", on);
}

bool Settings::nexus_queue_downloads() const {
  return settings_.value("nexus/queue_downloads", true).toBool();
}

void Settings::set_nexus_queue_downloads(bool on) {
  settings_.setValue("nexus/queue_downloads", on);
}

bool Settings::nexus_queue_downloads_set() const {
  return settings_.contains("nexus/queue_downloads");
}

void Settings::remove_nexus_queue_downloads() {
  settings_.remove("nexus/queue_downloads");
}

// workshop source ------------------------------------------------------------

int Settings::workshop_rate_limit_per_hour() const {
  return settings_.value("workshop/rate_limit_per_hour", 60).toInt();
}

void Settings::set_workshop_rate_limit_per_hour(int n) {
  settings_.setValue("workshop/rate_limit_per_hour", n);
}

// workarounds ------------------------------------------------------------------

QString Settings::executables_blacklist() const {
  return settings_.value("workarounds/executables_blacklist").toString();
}

void Settings::set_executables_blacklist(const QString &value) {
  settings_.setValue("workarounds/executables_blacklist", value);
}

QStringList Settings::skip_file_suffixes() const {
  return settings_.value("workarounds/skip_file_suffixes").toStringList();
}

void Settings::set_skip_file_suffixes(const QStringList &values) {
  settings_.setValue("workarounds/skip_file_suffixes", values);
}

QStringList Settings::skip_directories() const {
  return settings_.value("workarounds/skip_directories").toStringList();
}

void Settings::set_skip_directories(const QStringList &values) {
  settings_.setValue("workarounds/skip_directories", values);
}

// On by default: the plugin database has always locked the base game's own
// plugins and Creation Club content (they cannot be disabled or moved, always
// load and stay pinned above the user band), so "off" must be the opt-in.
// Releasing the lock is only safe for a total conversion that replaces those
// files - a game that needs Skyrim.esm and does not get it crashes.
bool Settings::force_enable_core_files() const {
  return settings_.value("game/force_enable_core_files", true).toBool();
}

void Settings::set_force_enable_core_files(bool on) {
  settings_.setValue("game/force_enable_core_files", on);
}

bool Settings::experimental_archive_parsing() const {
  return settings_.value("archive/experimental_parsing", false).toBool();
}

void Settings::set_experimental_archive_parsing(bool on) {
  settings_.setValue("archive/experimental_parsing", on);
}

void Settings::apply_workarounds() {
  engine::Workarounds w;
  // Trimmed on the way in: the field is free text, so ".esp , .esm" yields a
  // leading-space entry that would never match a filename.
  for (const auto &raw : skip_file_suffixes()) {
    const auto trimmed = raw.trimmed();
    if (!trimmed.isEmpty())
      w.hidden_suffixes.push_back(trimmed.toStdString());
  }
  for (const auto &raw : skip_directories()) {
    const auto trimmed = raw.trimmed();
    if (!trimmed.isEmpty())
      w.skipped_dirs.push_back(trimmed.toStdString());
  }
  engine::set_workarounds(std::move(w));
}

int Settings::overlay_capture_delay_ms() const {
  return settings_.value("overlay/capture_delay_ms", 3000).toInt();
}

void Settings::set_overlay_capture_delay_ms(int ms) {
  settings_.setValue("overlay/capture_delay_ms", ms);
}

// diagnostics ------------------------------------------------------------------

QString Settings::log_level() const {
  return settings_.value("diagnostics/log_level", "info").toString();
}

void Settings::set_log_level(const QString &level) {
  settings_.setValue("diagnostics/log_level", level);
}

int Settings::max_core_dumps() const {
  return settings_.value("diagnostics/max_core_dumps", 3).toInt();
}

void Settings::set_max_core_dumps(int n) {
  settings_.setValue("diagnostics/max_core_dumps", n);
}

// colors ----------------------------------------------------------------------

bool Settings::color_separator_scrollbar() const {
  return settings_.value("colors/color_separator_scrollbar", true).toBool();
}

void Settings::set_color_separator_scrollbar(bool on) {
  settings_.setValue("colors/color_separator_scrollbar", on);
}

bool Settings::center_separator_text() const {
  return settings_.value("appearance/center_separator_text", true).toBool();
}

void Settings::set_center_separator_text(bool on) {
  settings_.setValue("appearance/center_separator_text", on);
}

std::optional<QColor> Settings::previous_separator_color() const {
  if (!settings_.contains("colors/previous_separator_color"))
    return std::nullopt;
  auto c = settings_.value("colors/previous_separator_color").value<QColor>();
  if (!c.isValid())
    return std::nullopt;
  return c;
}

void Settings::set_previous_separator_color(const QColor &c) {
  settings_.setValue("colors/previous_separator_color", c);
}

void Settings::remove_previous_separator_color() {
  settings_.remove("colors/previous_separator_color");
}

QColor Settings::modlist_overwritten_loose() const {
  return settings_.value("colors/modlist_overwritten_loose", QColor(0, 255, 0, 64))
      .value<QColor>();
}

void Settings::set_modlist_overwritten_loose(const QColor &c) {
  settings_.setValue("colors/modlist_overwritten_loose", c);
}

QColor Settings::modlist_overwriting_loose() const {
  return settings_.value("colors/modlist_overwriting_loose", QColor(255, 0, 0, 64))
      .value<QColor>();
}

void Settings::set_modlist_overwriting_loose(const QColor &c) {
  settings_.setValue("colors/modlist_overwriting_loose", c);
}

QColor Settings::modlist_overwritten_archive() const {
  return settings_.value("colors/modlist_overwritten_archive", QColor(0, 255, 255, 64))
      .value<QColor>();
}

void Settings::set_modlist_overwritten_archive(const QColor &c) {
  settings_.setValue("colors/modlist_overwritten_archive", c);
}

QColor Settings::modlist_overwriting_archive() const {
  return settings_.value("colors/modlist_overwriting_archive", QColor(255, 0, 255, 64))
      .value<QColor>();
}

void Settings::set_modlist_overwriting_archive(const QColor &c) {
  settings_.setValue("colors/modlist_overwriting_archive", c);
}

QColor Settings::modlist_contains_file() const {
  return settings_.value("colors/modlist_contains_file", QColor(0, 0, 255, 64))
      .value<QColor>();
}

void Settings::set_modlist_contains_file(const QColor &c) {
  settings_.setValue("colors/modlist_contains_file", c);
}

QColor Settings::plugin_list_contained() const {
  return settings_.value("colors/plugin_list_contained", QColor(0, 0, 255, 64))
      .value<QColor>();
}

void Settings::set_plugin_list_contained(const QColor &c) {
  settings_.setValue("colors/plugin_list_contained", c);
}

QColor Settings::plugin_list_master() const {
  return settings_.value("colors/plugin_list_master", QColor(255, 255, 0, 64))
      .value<QColor>();
}

void Settings::set_plugin_list_master(const QColor &c) {
  settings_.setValue("colors/plugin_list_master", c);
}

// plugins ----------------------------------------------------------------------

QStringList Settings::disabled_plugins() const {
  return settings_.value("plugins/disabled").toStringList();
}

void Settings::set_disabled_plugins(const QStringList &names) {
  settings_.setValue("plugins/disabled", names);
}

bool Settings::plugin_enabled(const QString &name) const {
  return !disabled_plugins().contains(name);
}

void Settings::set_plugin_enabled(const QString &name, bool enabled) {
  auto disabled = disabled_plugins();
  if (enabled) {
    disabled.removeAll(name);
  } else if (!disabled.contains(name)) {
    disabled.append(name);
  }
  set_disabled_plugins(disabled);
}

QString Settings::plugin_setting(const QString &basename, const QString &key,
                                 const QString &default_value) const {
  return settings_
      .value(QString("plugins/settings/%1/%2").arg(basename, key), default_value)
      .toString();
}

void Settings::set_plugin_setting(const QString &basename, const QString &key,
                                  const QString &value) {
  settings_.setValue(QString("plugins/settings/%1/%2").arg(basename, key), value);
}

// nxm -------------------------------------------------------------------------

QString Settings::nxm_handler_check() const {
  return settings_.value("nxm/handler_check").toString();
}

void Settings::set_nxm_handler_check(const QString &value) {
  settings_.setValue("nxm/handler_check", value);
}

// fomod -----------------------------------------------------------------------

bool Settings::always_restore_fomod_choices() const {
  return settings_.value("fomod/always_restore_choices", true).toBool();
}

void Settings::set_always_restore_fomod_choices(bool on) {
  settings_.setValue("fomod/always_restore_choices", on);
}

bool Settings::show_fomod_images() const {
  return settings_.value("fomod/show_images", true).toBool();
}

void Settings::set_show_fomod_images(bool on) {
  settings_.setValue("fomod/show_images", on);
}

QByteArray Settings::fomod_window_geometry() const {
  return settings_.value("fomod/window_geometry").toByteArray();
}

void Settings::set_fomod_window_geometry(const QByteArray &g) {
  settings_.setValue("fomod/window_geometry", g);
}

QByteArray Settings::fomod_center_split() const {
  return settings_.value("fomod/center_split").toByteArray();
}

void Settings::set_fomod_center_split(const QByteArray &s) {
  settings_.setValue("fomod/center_split", s);
}

QByteArray Settings::fomod_left_split() const {
  return settings_.value("fomod/left_split").toByteArray();
}

void Settings::set_fomod_left_split(const QByteArray &s) {
  settings_.setValue("fomod/left_split", s);
}

QByteArray Settings::modinfo_window_geometry() const {
  return settings_.value("modinfo/window_geometry").toByteArray();
}

void Settings::set_modinfo_window_geometry(const QByteArray &g) {
  settings_.setValue("modinfo/window_geometry", g);
}

QList<int> Settings::modinfo_tab_order() const {
  QList<int> out;
  for (const auto &raw : settings_.value("modinfo/tab_order").toStringList()) {
    bool ok      = false;
    const int id = raw.toInt(&ok);
    if (ok)
      out.append(id);
  }
  return out;
}

void Settings::set_modinfo_tab_order(const QList<int> &order) {
  QStringList ids;
  ids.reserve(order.size());
  for (int id : order)
    ids << QString::number(id);
  settings_.setValue("modinfo/tab_order", ids);
}

QByteArray Settings::listdialog_window_geometry() const {
  return settings_.value("listdialog/window_geometry").toByteArray();
}

void Settings::set_listdialog_window_geometry(const QByteArray &g) {
  settings_.setValue("listdialog/window_geometry", g);
}

QByteArray Settings::saveinfo_window_geometry() const {
  return settings_.value("saveinfo/window_geometry").toByteArray();
}

void Settings::set_saveinfo_window_geometry(const QByteArray &g) {
  settings_.setValue("saveinfo/window_geometry", g);
}

int Settings::modinfo_last_tab() const {
  return settings_.value("modinfo/last_tab", 0).toInt();
}

void Settings::set_modinfo_last_tab(int index) {
  settings_.setValue("modinfo/last_tab", index);
}

// preview -------------------------------------------------------------------

int Settings::checkerboard_style() const {
  const int style = settings_.value("preview/checkerboard_style", 2).toInt();
  return std::clamp(style, 0, 3);
}

void Settings::set_checkerboard_style(int style) {
  settings_.setValue("preview/checkerboard_style", std::clamp(style, 0, 3));
}
