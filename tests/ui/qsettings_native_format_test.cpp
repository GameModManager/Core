// Workspace-2zm: proof that the production QSettings expression is UNCHANGED
// for real users.
//
// This target is registered with GMM_TEST_NATIVE_QSETTINGS=1, the opt-out in
// tests/support/test_main.cpp: the shared runner skips its sandbox, so this
// process sees exactly what a real run sees - QSettings::defaultFormat() is
// whatever it is before anything touches it.
//
// It asserts only on format() and fileName(). QSettings is lazy (no file is
// created until a value is written or sync() is called), so this test never
// reads, creates or modifies the user's config - it compares PATHS.

#include <QDir>
#include <QSettings>
#include <QString>

#include <catch2/catch_test_macros.hpp>

namespace {
// The production expression, verbatim: src/ui/settings/settings.h and both
// plugin_settings_registry.cpp sites build their QSettings this way.
QSettings production_settings() {
  return QSettings(QSettings::defaultFormat(), QSettings::UserScope,
                   "GameModManager", "GameModManager");
}
}  // namespace

TEST_CASE("qsettings: production format is native, not the test sandbox",
          "[qsettings]") {
  REQUIRE(QSettings::defaultFormat() == QSettings::NativeFormat);

  QSettings s = production_settings();
  REQUIRE(s.format() == QSettings::NativeFormat);
  REQUIRE(s.scope() == QSettings::UserScope);
  REQUIRE(s.organizationName() == "GameModManager");
  REQUIRE(s.applicationName() == "GameModManager");
  // A real user's store, not a test sandbox.
  REQUIRE(!s.fileName().startsWith(QDir::tempPath()));
}

TEST_CASE("qsettings: production expression addresses the pre-change file",
          "[qsettings]") {
  // The load-bearing proof that real users are unaffected: the new explicit
  // -format ctor and the (organization, application) ctor it replaced resolve
  // to the SAME file. Same file + same org/app + same format == byte-identical
  // storage, so existing settings are read and written exactly as before.
  // (Qt docs: the org/app ctor is UserScope + NativeFormat; passing
  // NativeFormat explicitly selects the same path on every platform.)
  const QString before = QSettings("GameModManager", "GameModManager").fileName();
  const QString after = production_settings().fileName();
  INFO("pre-change store: " << before.toStdString());
  INFO("post-change store: " << after.toStdString());
  REQUIRE(after == before);
}
