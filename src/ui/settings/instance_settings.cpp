#include "ui/settings/instance_settings.h"

#include <QCoreApplication>
#include <QProcess>

#include "engine/core/instance/instance.h"
#include "engine/core/log/logger.h"
#include "ui/settings/settings.h"

namespace ui {

namespace {

  bool have_root(const std::filesystem::path &instance_root) {
    return !instance_root.empty();
  }

}  // namespace

InstanceAppearance
read_instance_appearance(const std::filesystem::path &instance_root) {
  InstanceAppearance out;
  if (!have_root(instance_root))
    return out;
  engine::Instance inst = engine::Instance::from_root(instance_root);
  if (!inst.read_toml())
    return out;
  out.theme     = QString::fromStdString(inst.info().appearance_theme);
  out.style     = QString::fromStdString(inst.info().appearance_style);
  out.icon_pack = QString::fromStdString(inst.info().appearance_icon_pack);
  return out;
}

InstancePluginSettings
read_instance_plugin_settings(const std::filesystem::path &instance_root) {
  InstancePluginSettings out;
  if (!have_root(instance_root))
    return out;
  engine::Instance inst = engine::Instance::from_root(instance_root);
  if (!inst.read_toml())
    return out;
  if (inst.info().plugins_disabled.has_value()) {
    QStringList disabled;
    for (const auto &name : *inst.info().plugins_disabled)
      disabled.append(QString::fromStdString(name));
    out.disabled = disabled;
  }
  for (const auto &[basename, settings] : inst.info().plugin_options) {
    const QString base = QString::fromStdString(basename);
    for (const auto &[key, value] : settings)
      out.options[base][QString::fromStdString(key)] = QString::fromStdString(value);
  }
  return out;
}

QString effective_theme(const std::filesystem::path &instance_root) {
  const QString local = read_instance_appearance(instance_root).theme;
  if (!local.isEmpty())
    return local;
  return Settings::instance().theme();
}

QString effective_style(const std::filesystem::path &instance_root) {
  const QString local = read_instance_appearance(instance_root).style;
  if (!local.isEmpty())
    return local;
  return Settings::instance().style();
}

QString effective_icon_pack(const std::filesystem::path &instance_root) {
  const QString local = read_instance_appearance(instance_root).icon_pack;
  if (!local.isEmpty())
    return local;
  return Settings::instance().icon_pack();
}

QStringList effective_disabled_plugins(const std::filesystem::path &instance_root) {
  const auto local = read_instance_plugin_settings(instance_root).disabled;
  if (local.has_value())
    return *local;
  return Settings::instance().disabled_plugins();
}

bool effective_plugin_enabled(const std::filesystem::path &instance_root,
                              const QString &basename) {
  return !effective_disabled_plugins(instance_root).contains(basename);
}

QString effective_plugin_setting(const std::filesystem::path &instance_root,
                                 const QString &basename, const QString &key,
                                 const QString &default_value) {
  const auto opts = read_instance_plugin_settings(instance_root).options;
  const auto it   = opts.find(basename);
  if (it != opts.end()) {
    const auto jt = it->find(key);
    if (jt != it->end())
      return *jt;
  }
  return Settings::instance().plugin_setting(basename, key, default_value);
}

void set_instance_theme(const std::filesystem::path &instance_root,
                        const QString &value) {
  if (!have_root(instance_root))
    return;
  engine::Instance inst = engine::Instance::from_root(instance_root);
  inst.read_toml();
  inst.info().appearance_theme = value.toStdString();
  if (!inst.write_toml())
    engine::Logger::instance().warn("Failed to write per-instance theme to " +
                                    instance_root.string());
}

void set_instance_style(const std::filesystem::path &instance_root,
                        const QString &value) {
  if (!have_root(instance_root))
    return;
  engine::Instance inst = engine::Instance::from_root(instance_root);
  inst.read_toml();
  inst.info().appearance_style = value.toStdString();
  if (!inst.write_toml())
    engine::Logger::instance().warn("Failed to write per-instance style to " +
                                    instance_root.string());
}

void set_instance_icon_pack(const std::filesystem::path &instance_root,
                            const QString &value) {
  if (!have_root(instance_root))
    return;
  engine::Instance inst = engine::Instance::from_root(instance_root);
  inst.read_toml();
  inst.info().appearance_icon_pack = value.toStdString();
  if (!inst.write_toml())
    engine::Logger::instance().warn("Failed to write per-instance icon pack to " +
                                    instance_root.string());
}

void set_instance_plugin_enabled(const std::filesystem::path &instance_root,
                                 const QString &basename, bool enabled) {
  if (!have_root(instance_root))
    return;
  engine::Instance inst = engine::Instance::from_root(instance_root);
  inst.read_toml();
  // Seed from the effective list so toggling one plugin preserves the rest
  // (including entries that currently only exist as globals).
  QStringList disabled =
      inst.info().plugins_disabled.has_value()
          ? read_instance_plugin_settings(instance_root).disabled.value()
          : Settings::instance().disabled_plugins();
  if (enabled) {
    disabled.removeAll(basename);
  } else if (!disabled.contains(basename)) {
    disabled.append(basename);
  }
  std::vector<std::string> names;
  for (const auto &name : disabled)
    names.push_back(name.toStdString());
  inst.info().plugins_disabled = std::move(names);
  if (!inst.write_toml())
    engine::Logger::instance().warn(
        "Failed to write per-instance disabled plugins to " + instance_root.string());
}

void set_instance_plugin_setting(const std::filesystem::path &instance_root,
                                 const QString &basename, const QString &key,
                                 const QString &value) {
  if (!have_root(instance_root))
    return;
  engine::Instance inst = engine::Instance::from_root(instance_root);
  inst.read_toml();
  inst.info().plugin_options[basename.toStdString()][key.toStdString()] =
      value.toStdString();
  if (!inst.write_toml())
    engine::Logger::instance().warn("Failed to write per-instance plugin option to " +
                                    instance_root.string());
}

bool instance_effective_settings_differ(const std::filesystem::path &from,
                                        const std::filesystem::path &to) {
  if (effective_theme(from) != effective_theme(to))
    return true;
  if (effective_style(from) != effective_style(to))
    return true;
  if (effective_icon_pack(from) != effective_icon_pack(to))
    return true;
  if (effective_disabled_plugins(from) != effective_disabled_plugins(to))
    return true;
  // Plugin options: compare the union of locally overridden keys with
  // global fallback applied on each side.
  const auto from_opts = read_instance_plugin_settings(from).options;
  const auto to_opts   = read_instance_plugin_settings(to).options;
  for (const auto &opts : {from_opts, to_opts}) {
    for (auto it = opts.begin(); it != opts.end(); ++it) {
      for (auto jt = it->begin(); jt != it->end(); ++jt) {
        const QString fallback =
            Settings::instance().plugin_setting(it.key(), jt.key(), QString());
        const auto fit         = from_opts.find(it.key());
        const QString from_val = (fit != from_opts.end() && fit->contains(jt.key()))
                                     ? fit->value(jt.key())
                                     : fallback;
        const auto tit         = to_opts.find(it.key());
        const QString to_val   = (tit != to_opts.end() && tit->contains(jt.key()))
                                     ? tit->value(jt.key())
                                     : fallback;
        if (from_val != to_val)
          return true;
      }
    }
  }
  return false;
}

void restart_application(const QStringList &extra_args) {
  QStringList args = QCoreApplication::arguments();
  if (!args.isEmpty())
    args.removeFirst();  // argv[0] is re-added by startDetached
  // Drop already-consumed download-handler flags: relaunching with them
  // would re-trigger the download flow (duplicate handling).
  for (const QString &flag : {"--handle-nxm", "--handle-gmm", "--handle-modl"}) {
    for (int i = args.size() - 1; i >= 0; --i) {
      if (args[i] == flag) {
        args.removeAt(i);
        if (i < args.size() && !args[i].startsWith("--"))
          args.removeAt(i);
      } else if (args[i].startsWith(flag + "=")) {
        args.removeAt(i);
      }
    }
  }
  args += extra_args;
  engine::Logger::instance().info(
      "Restarting: " + QCoreApplication::applicationFilePath().toStdString());
  QProcess::startDetached(QCoreApplication::applicationFilePath(), args);
  QCoreApplication::quit();
}

}  // namespace ui
