// Workspace-2zm: proof that PRODUCTION code - not a copy of the production
// expression - keeps the same QSettings store for real users.
//
// This target is registered with GMM_TEST_NATIVE_QSETTINGS=1, the opt-out in
// tests/support/test_main.cpp: the shared runner skips its sandbox, so this
// process sees exactly what a real run sees - QSettings::defaultFormat() is
// whatever it is before anything touches it. The real ui::Settings singleton
// and the real engine::PluginSettingsRegistry singletons are exercised
// directly, so reverting either production constructor is what these cases
// report on; nothing here asserts against a hand-written stand-in.
//
// SAFETY. This process deliberately escapes the sandbox, and unlike the old
// path-only version of this file it now WRITES. Before either case constructs
// anything it relocates the NATIVE store root into a throwaway temp dir with
// QSettings::setPath - the same thing a fake HOME would achieve, except an
// inherited XDG_CONFIG_HOME cannot defeat it and Qt does not cache it. The
// first case additionally gates on the resolved store actually being inside
// that throwaway, so a platform whose native backend ignores setPath (macOS
// CFPreferences, Windows registry) SKIPS instead of touching a real user's
// config. Two independent guards, on purpose: a prior agent damaged a real
// config by running this binary bare.
//
// MUTATION RUNS. Deliberately reverting a production constructor makes the
// reverted site reach the native store, which is what makes the second case
// fail - and that is precisely when a leak would happen. Both cases therefore
// relocate the NATIVE root too, and the mutation proof itself is run with the
// whole ctest invocation under a throwaway XDG_CONFIG_HOME/HOME:
//
//   XDG_CONFIG_HOME=/tmp/gmm_fake_home HOME=/tmp/gmm_fake_home \
//     ctest --test-dir build -R qsettings

#include "ui/settings/settings.h"

#include "engine/pipeline/plugin_host/plugin_settings_registry.h"

#include <QDir>
#include <QSettings>
#include <QString>
#include <QUuid>

#include <catch2/catch_test_macros.hpp>

namespace {
// The READ side of the proof: a fresh handle pinned to NativeFormat. It is not
// the production expression - production decides its own format - it is the
// oracle the production writes are read back through.
QSettings native_store() {
  return QSettings(QSettings::NativeFormat, QSettings::UserScope,
                   "GameModManager", "GameModManager");
}
}  // namespace

TEST_CASE("qsettings: production objects write the pre-change native store",
          "[qsettings]") {
  // The trap guard: if anyone ever adds a setDefaultFormat() to production (or
  // to a static initializer that runs before the shared runner), real users
  // would silently move store and this is the case that says so.
  REQUIRE(QSettings::defaultFormat() == QSettings::NativeFormat);

  // Fake native root, first thing in the process to touch QSettings. Unique per
  // run so two runs never read each other's probe values.
  const QString throwaway = QDir::tempPath() + "/gmm_qsettings_native_" +
                            QUuid::createUuid().toString(QUuid::WithoutBraces);
  QDir().mkpath(throwaway);
  QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, throwaway);

  QSettings store = native_store();
  INFO("throwaway native root: " << throwaway.toStdString());
  INFO("native store: " << store.fileName().toStdString());

  // SAFETY GATE, before a single production object is constructed: the resolved
  // store must be inside the throwaway root. Skipping here is strictly better
  // than writing to a real user's config.
  if (!store.fileName().startsWith(throwaway + "/"))
    SKIP("setPath(NativeFormat) was ignored and the store resolved outside the "
         "throwaway root; refusing to write (see the INFO lines above)");

  // The genuine native layout, just rooted in the throwaway: this is the file a
  // real user has always had, with the same org/app suffix.
  REQUIRE(store.fileName() ==
          throwaway + "/GameModManager/GameModManager.conf");
  REQUIRE(store.format() == QSettings::NativeFormat);
  REQUIRE(store.scope() == QSettings::UserScope);
  REQUIRE(store.organizationName() == "GameModManager");
  REQUIRE(store.applicationName() == "GameModManager");

  // WRITE THROUGH PRODUCTION, both sites: the ui::Settings facade
  // (src/ui/settings/settings.h) and the engine registry
  // (plugin_settings_registry.cpp set_setting), which the host callbacks and
  // the Plugins tab both go through.
  Settings::instance().set_language("zz-native-probe");
  engine::PluginSettingsRegistry &registry =
      engine::PluginSettingsRegistry::instance();
  registry.set_setting("ZzNativeProbePlugin.so", "probe_key", "probe_value");
  store.sync();

  // READ BACK through a brand new handle, so the values can only have come
  // from the file the production objects just wrote.
  QSettings fresh = native_store();
  REQUIRE(fresh.value("language").toString() == "zz-native-probe");
  REQUIRE(fresh.value("plugins/settings/ZzNativeProbePlugin.so/probe_key")
              .toString() == "probe_value");
  REQUIRE(registry.get_setting("ZzNativeProbePlugin.so", "probe_key") ==
          "probe_value");

  // The load-bearing proof that real users are unaffected, now with a real file
  // on disk instead of a path string: the constructor production replaced -
  // QSettings(organization, application) - reads back everything production
  // wrote. Same file + same org/app + same format == byte-identical storage, so
  // existing settings keep working with no migration.
  QSettings prechange("GameModManager", "GameModManager");
  REQUIRE(prechange.fileName() == store.fileName());
  REQUIRE(prechange.value("language").toString() == "zz-native-probe");
  REQUIRE(prechange.value("plugins/settings/ZzNativeProbePlugin.so/probe_key")
              .toString() == "probe_value");

  // No trailing cleanup: the production QSettings objects are still alive and
  // their destructors sync after this case returns, which would recreate the
  // file. The root is UUID-named and under the system temp dir, so a leftover
  // costs nothing and never collides with the next run.
}

TEST_CASE("qsettings: production objects read the process default format",
          "[qsettings]") {
  // The complement of the case above, and the one that has teeth when a
  // production site is reverted. NativeFormat cannot tell the two constructors
  // apart - both address the same file, which is exactly the property the first
  // case certifies - so discriminating them needs a process whose default
  // format is not native. This case owns that flip itself.
  const QString dir = QDir::tempPath() + "/gmm_qsettings_native_probe";
  QDir(dir).removeRecursively();
  QDir().mkpath(dir);
  // BOTH roots, not just the ini one. The whole point of the case is a
  // production site that IGNORES the flipped default format and reaches for the
  // native store instead - which, unrelocated, is the user's real config.
  // Pointing NativeFormat at the same throwaway contains that failure inside
  // /tmp instead of in a real home; the assertions below still see it, because
  // the ini read-back comes up empty either way.
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir);
  QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir);
  QSettings::setDefaultFormat(QSettings::IniFormat);

  Settings::instance().set_language("zz-format-probe");
  engine::PluginSettingsRegistry::instance().set_setting(
      "ZzFormatProbePlugin.so", "probe_key", "probe_value");

  QSettings ini(QSettings::IniFormat, QSettings::UserScope, "GameModManager",
                "GameModManager");
  REQUIRE(ini.fileName().startsWith(dir));
  REQUIRE(ini.value("language").toString() == "zz-format-probe");
  REQUIRE(ini.value("plugins/settings/ZzFormatProbePlugin.so/probe_key")
              .toString() == "probe_value");

  // No trailing cleanup: the production QSettings objects are still alive and
  // their destructors sync after this case returns, which would recreate the
  // file. The leading removeRecursively() keeps it to one throwaway dir, and
  // ctest gives each case its own process so this flip cannot leak into the
  // native case above (declaration order covers the manual both-cases run).
}
