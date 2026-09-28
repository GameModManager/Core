// Workspace-2zm: proof that the production QSettings expression is redirected
// into the shared test sandbox (tests/support/test_main.cpp) instead of the
// user's real config.
//
// Production now builds its QSettings with the format as an explicit argument
// (QSettings::defaultFormat(), QSettings::UserScope, org, app) at the three
// sites: src/ui/settings/settings.h, plugin_settings_registry.cpp get_setting
// and set_setting. The shared runner calls setDefaultFormat(IniFormat) +
// setPath(IniFormat, UserScope, tmpdir) before QApplication, so in a test
// process that expression resolves to a temp .ini and never to the user's
// store. The sibling test qsettings_native_format_test covers the production
// (non-sandboxed) side: same expression, NativeFormat, same file as before.

#include "ui/settings/settings.h"

#include "engine/pipeline/plugin_host/plugin_settings_registry.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QString>

#include <catch2/catch_test_macros.hpp>

namespace {

// The production expression, verbatim. Kept in a helper so this test exercises
// the same shape the three production sites use.
QSettings production_settings() {
  return QSettings(QSettings::defaultFormat(), QSettings::UserScope,
                   "GameModManager", "GameModManager");
}

// Snapshot of a real user config file, so a sandboxed run can prove it did not
// write to it. Comparing (exists, size, mtime) avoids asserting "the file must
// not exist", which would be a false failure on a machine that legitimately has
// one.
struct FileStamp {
  bool exists = false;
  qint64 size = -1;
  QDateTime mtime;

  static FileStamp of(const QString &path) {
    const QFileInfo info(path);
    FileStamp s;
    s.exists = info.exists();
    s.size = info.size();
    s.mtime = info.lastModified();
    return s;
  }
  bool operator==(const FileStamp &o) const {
    return exists == o.exists && size == o.size && mtime == o.mtime;
  }
};

}  // namespace

TEST_CASE("qsettings: production expression is the sandboxed ini in tests",
          "[ui][qsettings]") {
  // The sandbox only exists because the shared runner flipped the default
  // format; if that ever regresses this is the first test to say so.
  REQUIRE(QSettings::defaultFormat() == QSettings::IniFormat);

  QSettings s = production_settings();
  REQUIRE(s.format() == QSettings::IniFormat);
  REQUIRE(s.organizationName() == "GameModManager");
  REQUIRE(s.applicationName() == "GameModManager");
  // Same org/app names as production means the same keys are exercised; only
  // the storage backend differs.
  REQUIRE(s.fileName().startsWith(QDir::tempPath()));

  // The legacy (organization, application) constructor resolves to a
  // DIFFERENT store inside a test process. That divergence is the whole point:
  // it means the redirect is real and not the pre-existing Unix-only
  // XDG_CONFIG_HOME coincidence.
  const QString legacy =
      QSettings("GameModManager", "GameModManager").fileName();
  REQUIRE(legacy != s.fileName());
}

TEST_CASE("qsettings: a sandboxed test cannot write the user's real config",
          "[ui][qsettings]") {
  // Both native stores a user could have, on Unix and on macOS. Nothing here
  // reads or redirects them - this is purely a "did anything touch it?" probe.
  const QString real_native = QDir::homePath() +
                              "/.config/GameModManager/GameModManager.conf";
  const QString real_plist = QDir::homePath() +
                             "/Library/Preferences/com.GameModManager."
                             "GameModManager.plist";
  const FileStamp before_native = FileStamp::of(real_native);
  const FileStamp before_plist = FileStamp::of(real_plist);

  // Write through the production facade (this is the path a UI test exercises
  // when it toggles a checkbox).
  Settings::instance().set_language("zz-sandbox-probe");
  Settings::instance().nxm_handler_check();

  // And through the two ENGINE sites, which build the same QSettings
  // (plugin_settings_registry.cpp get_setting / set_setting). Both write under
  // plugins/settings/<basename>/<key>, the keys the Plugins tab edits.
  engine::PluginSettingsRegistry &registry =
      engine::PluginSettingsRegistry::instance();
  registry.set_setting("ZzProbePlugin.so", "probe_key", "probe_value");

  // Read every write back through the production expression. A site that went
  // back to the (organization, application) constructor would write to the
  // native store and these read-backs would miss - that is the regression this
  // test exists to catch, so it reads the real production objects, not a copy
  // of the expression.
  QSettings s = production_settings();
  REQUIRE(s.value("language").toString() == "zz-sandbox-probe");
  REQUIRE(s.value("plugins/settings/ZzProbePlugin.so/probe_key").toString() ==
          "probe_value");
  REQUIRE(registry.get_setting("ZzProbePlugin.so", "probe_key") ==
          "probe_value");
  // The file backing all of it is inside the sandbox, not the user's config.
  REQUIRE(s.fileName().startsWith(QDir::tempPath()));
  REQUIRE(QFileInfo::exists(s.fileName()));

  REQUIRE(FileStamp::of(real_native) == before_native);
  REQUIRE(FileStamp::of(real_plist) == before_plist);
}
