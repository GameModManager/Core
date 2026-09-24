#pragma once

// Per-instance appearance/plugin settings (Workspace-1065, Workspace-jagw).
//
// instance.toml may carry [appearance] (theme/style/icon_pack), [plugins]
// (disabled = [...]) and [plugin_options] (nested basename tables) sections.
// Every reader here falls back to the global Settings value when the
// per-instance key is absent: an empty appearance string, a nullopt disabled
// list, or a missing option entry all mean "use the global".
//
// The engine (engine::Instance::Info) owns the TOML schema; this module owns
// the Qt-facing effective-value resolution, the write-through helpers used by
// the settings dialog, and the restart helper used on instance switch.

#include <QMap>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <optional>

namespace ui {

// Raw per-instance appearance overrides (empty = unset = global fallback).
struct InstanceAppearance {
  QString theme;
  QString style;
  QString icon_pack;
};

// Raw per-instance plugin settings (disabled nullopt = global fallback;
// options only holds explicit overrides, missing entries fall back).
struct InstancePluginSettings {
  std::optional<QStringList> disabled;
  QMap<QString, QMap<QString, QString>> options;  // basename -> key -> value
};

InstanceAppearance read_instance_appearance(const std::filesystem::path &instance_root);
InstancePluginSettings
read_instance_plugin_settings(const std::filesystem::path &instance_root);

// Effective values: per-instance override when set, else the global
// Settings value. An empty instance_root always yields the globals.
QString effective_theme(const std::filesystem::path &instance_root);
QString effective_style(const std::filesystem::path &instance_root);
QString effective_icon_pack(const std::filesystem::path &instance_root);
QStringList effective_disabled_plugins(const std::filesystem::path &instance_root);
bool effective_plugin_enabled(const std::filesystem::path &instance_root,
                              const QString &basename);
QString effective_plugin_setting(const std::filesystem::path &instance_root,
                                 const QString &basename, const QString &key,
                                 const QString &default_value);

// Write-through helpers: read-modify-write on instance.toml (other keys
// preserved). An empty value clears the per-instance key (back to global
// fallback). Setting a plugin-enabled state seeds from the effective list
// so unrelated globals are preserved.
void set_instance_theme(const std::filesystem::path &instance_root,
                        const QString &value);
void set_instance_style(const std::filesystem::path &instance_root,
                        const QString &value);
void set_instance_icon_pack(const std::filesystem::path &instance_root,
                            const QString &value);
void set_instance_plugin_enabled(const std::filesystem::path &instance_root,
                                 const QString &basename, bool enabled);
void set_instance_plugin_setting(const std::filesystem::path &instance_root,
                                 const QString &basename, const QString &key,
                                 const QString &value);

// True when the effective appearance/plugin settings of two instances
// differ (i.e. a live switch would leave stale theme/icons/plugins).
bool instance_effective_settings_differ(const std::filesystem::path &from,
                                        const std::filesystem::path &to);

// Relaunch the app with the same CLI arguments (minus any --handle-*
// download flags, which were already consumed) plus extra_args, then quit.
// The caller is responsible for persisting state (e.g. MainWindow::close()
// first so closeEvent saves app state).
void restart_application(const QStringList &extra_args = {});

}  // namespace ui
