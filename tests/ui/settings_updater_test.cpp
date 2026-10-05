// The three updater settings, and the read-only line that says which update
// route this install has.
//
// A getter round-trip would prove nothing here: it passes with the consumer's
// body emptied. So each setting is asserted on what the consumer produces.
//
//   check_for_updates             -> the cadence line the panel renders
//   check_update_after_mod_install -> (main_window_harness_test.cpp) the mod
//                                     update database poll on install
//   use_prereleases               -> no consumer, on purpose: the control is
//                                     disabled and its tooltip names the
//                                     prerequisite, which is the only honest
//                                     state while there are no releases
#include "engine/theme/theme_manager.h"
#include "engine/plugin_host/plugin_loader.h"
#include "engine/update/install_method.h"
#include "ui/settings/settings.h"
#include "ui/settings/settings_content_widget.h"
#include "ui/theme/style_manager.h"

#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QSettings>
#include <QTabWidget>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace {

// Every case gets its own throwaway instance root so nothing reaches the
// user's data; QSettings itself is sandboxed process-wide by test_main.cpp.
struct Panel {
  engine::ThemeManager theme_manager;
  engine::StyleManager style{theme_manager};
  engine::PluginLoader loader;
  std::filesystem::path root;
  ui::SettingsContentWidget *widget = nullptr;

  Panel() {
    root = std::filesystem::temp_directory_path() / "gmm_settings_updater";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "instances" / "Test");
    widget = new ui::SettingsContentWidget(&style, "breeze", root, &loader);
  }
  ~Panel() {
    delete widget;
    std::filesystem::remove_all(root);
  }

  // Open the General tab so every control on it is built.
  void open_general() { widget->tab_widget()->setCurrentIndex(0); }

  QCheckBox *checkbox(const QString &label) const {
    for (auto *b : widget->findChildren<QCheckBox *>())
      if (b->text() == label)
        return b;
    return nullptr;
  }

  // The read-only install-method line.
  QLabel *install_line() const {
    for (auto *l : widget->findChildren<QLabel *>())
      if (l->text().startsWith(QStringLiteral("This install:")))
        return l;
    return nullptr;
  }

  // The read-only automatic-check cadence line.
  QLabel *cadence_line() const {
    for (auto *l : widget->findChildren<QLabel *>())
      if (l->text().startsWith(QStringLiteral("Automatic app update check:")))
        return l;
    return nullptr;
  }
};

constexpr const char *kCheckBoxAppUpdates = "Check for app updates every 24 hours";
constexpr const char *kCheckBoxPrerelease = "Use prerelease updates";
constexpr const char *kCheckBoxModUpdates = "Check for mods for updates after install";

}  // namespace

// ---------------------------------------------------------------------------
// The read-only line: what this install is, and therefore which route applies
// ---------------------------------------------------------------------------
TEST_CASE("settings: the panel reports this install and its update route",
          "[ui][settings][update]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int argc     = 1;
  char argv0[] = "settings_updater_test";
  char *argv[] = {argv0, nullptr};
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  Panel panel;
  panel.open_general();

  auto *line = panel.install_line();
  REQUIRE(line != nullptr);

  // It says what the detector found, by name and by route, so the user can
  // see why the app will or will not update itself.
  const auto method =
      engine::update::detect_install_method(engine::update::probe_install_facts());
  INFO("detected method: " << engine::update::install_method_name(method));
  CHECK(line->text().contains(
      QString::fromUtf8(engine::update::install_method_name(method))));
  CHECK(line->text().contains(
      QString::fromUtf8(engine::update::install_method_route(method))));

  // And it does not promise what this build cannot do. Without this the line
  // would read as a live updater.
  CHECK(line->text().contains(QStringLiteral("does not run it")));

  // Read-only: selectable text, no buddy control, nothing to click.
  CHECK(line->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));
  CHECK(line->buddy() == nullptr);
  CHECK(line->isEnabled());

  // Exactly one such line, so a second copy cannot shadow the first.
  int lines = 0;
  for (auto *l : panel.widget->findChildren<QLabel *>())
    if (l->text().startsWith(QStringLiteral("This install:")))
      ++lines;
  CHECK(lines == 1);
}

// ---------------------------------------------------------------------------
// check_for_updates -> the cadence line. Consumer output, not the getter.
// ---------------------------------------------------------------------------
TEST_CASE("settings: check_for_updates changes what the cadence line says",
          "[ui][settings][update]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int argc     = 1;
  char argv0[] = "settings_updater_test";
  char *argv[] = {argv0, nullptr};
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  auto &s = Settings::instance();

  // Stored off (the shipped default: an automatic check is a third-party
  // request on every launch, which the user did not ask for).
  s.set_check_for_updates(false);
  {
    Panel panel;
    panel.open_general();
    auto *line = panel.cadence_line();
    REQUIRE(line != nullptr);
    INFO("with the setting stored off: " << line->text().toStdString());
    CHECK(line->text().contains(QStringLiteral("off")));
  }

  // Stored on, never checked: the line names the cadence and the absence of a
  // previous check.
  s.set_check_for_updates(true);
  s.set_last_update_check(QDateTime());
  {
    Panel panel;
    panel.open_general();
    auto *line = panel.cadence_line();
    REQUIRE(line != nullptr);
    INFO("with the setting stored on: " << line->text().toStdString());
    CHECK(line->text().contains(QStringLiteral("24 hours")));
    CHECK(line->text().contains(QStringLiteral("Never checked yet")));
  }

  // Stored on, checked once: the line reports when. This is what makes the
  // stored timestamp mean something.
  s.set_last_update_check(QDateTime(QDate(2026, 3, 14), QTime(9, 30)));
  {
    Panel panel;
    panel.open_general();
    auto *line = panel.cadence_line();
    REQUIRE(line != nullptr);
    INFO("with a stored check time: " << line->text().toStdString());
    CHECK(line->text().contains(QStringLiteral("Last checked")));
    CHECK(line->text().contains(QStringLiteral("2026")));
  }

  // The consumer is live: a real click on the checkbox rewrites the line,
  // without rebuilding the panel. This is the assertion a getter round-trip
  // cannot make.
  s.set_check_for_updates(false);
  {
    Panel panel;
    panel.open_general();
    auto *box  = panel.checkbox(QString::fromUtf8(kCheckBoxAppUpdates));
    auto *line = panel.cadence_line();
    REQUIRE(box != nullptr);
    REQUIRE(line != nullptr);
    REQUIRE_FALSE(box->isChecked());
    const QString before = line->text();
    QTest::mouseClick(box, Qt::LeftButton);
    QCoreApplication::processEvents();
    INFO("before: " << before.toStdString());
    INFO("after:  " << line->text().toStdString());
    CHECK(box->isChecked());
    CHECK(line->text() != before);
    CHECK(line->text().contains(QStringLiteral("24 hours")));

    // And back off again, so the effect is the toggle and not a one-way latch.
    QTest::mouseClick(box, Qt::LeftButton);
    QCoreApplication::processEvents();
    CHECK(line->text() == before);
  }
}

// ---------------------------------------------------------------------------
// use_prereleases: no consumer, and the control says so
// ---------------------------------------------------------------------------
TEST_CASE("settings: use_prereleases is disabled and names its prerequisite",
          "[ui][settings][update]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int argc     = 1;
  char argv0[] = "settings_updater_test";
  char *argv[] = {argv0, nullptr};
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  Panel panel;
  panel.open_general();
  auto *box = panel.checkbox(QString::fromUtf8(kCheckBoxPrerelease));
  REQUIRE(box != nullptr);

  INFO("tooltip: " << box->toolTip().toStdString());
  CHECK_FALSE(box->isEnabled());
  // The tooltip has to name the thing that is missing, not just say it is
  // unavailable: there are no releases and no channel concept.
  CHECK(box->toolTip().contains(QStringLiteral("no GitHub releases")));
  CHECK(box->toolTip().contains(QStringLiteral("prerelease")));
  CHECK(box->toolTip().contains(QStringLiteral("Unavailable")));
}

// ---------------------------------------------------------------------------
// check_update_after_mod_install: the label must not read as an app update
// ---------------------------------------------------------------------------
TEST_CASE("settings: the after-install check is labelled as a mod check",
          "[ui][settings][update]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int argc     = 1;
  char argv0[] = "settings_updater_test";
  char *argv[] = {argv0, nullptr};
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  Panel panel;
  panel.open_general();
  auto *box = panel.checkbox(QString::fromUtf8(kCheckBoxModUpdates));
  REQUIRE(box != nullptr);

  INFO("tooltip: " << box->toolTip().toStdString());
  CHECK(box->toolTip().contains(QStringLiteral("mod update database")));
  CHECK(box->toolTip().contains(QStringLiteral("not an application update")));
  // The rename is real: the old label is gone, so the control cannot be read
  // as an application update.
  CHECK(panel.checkbox(QStringLiteral("Check for updates after install")) == nullptr);

  // The setter writes the renamed key, and only that one.
  QSettings probe;
  probe.remove(QStringLiteral("interface/check_update_after_mod_install"));
  probe.remove(QStringLiteral("interface/check_update_after_install"));
  probe.sync();
  Settings::instance().set_check_update_after_mod_install(false);
  probe.sync();
  CHECK(probe.value(QStringLiteral("interface/check_update_after_mod_install"))
            .toBool() == false);
  CHECK(!probe.contains(QStringLiteral("interface/check_update_after_install")));
}

// ---------------------------------------------------------------------------
// The migration: an existing stored value must survive the rename
// ---------------------------------------------------------------------------
TEST_CASE("settings: an existing check_update_after_install value survives the rename",
          "[ui][settings][update]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int argc     = 1;
  char argv0[] = "settings_updater_test";
  char *argv[] = {argv0, nullptr};
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  auto &s = Settings::instance();
  QSettings probe;

  const QString old_key(QStringLiteral("interface/check_update_after_install"));
  const QString new_key(QStringLiteral("interface/check_update_after_mod_install"));

  // A user who had turned the old setting OFF before the rename. The default
  // is ON, so returning the default here would silently flip their choice.
  {
    probe.remove(new_key);
    probe.setValue(old_key, false);
    probe.sync();
    INFO("old key stored as false");
    CHECK_FALSE(s.check_update_after_mod_install());
  }

  // The same for a stored ON, against a default of ON: only a key that is
  // present may decide, so the false case above is the one that matters and
  // this pins the true case too.
  {
    probe.setValue(old_key, true);
    probe.sync();
    CHECK(s.check_update_after_mod_install());
  }

  // Reading migrated nothing: the old key is still on disk, untouched. A
  // read that rewrote or removed the old key could not be distinguished from
  // one that had, and could lose the value if it were removed after a partial
  // write.
  CHECK(probe.contains(old_key));
  CHECK_FALSE(probe.contains(new_key));

  // Once the user touches the checkbox the new key wins from then on, in
  // whichever direction, and the old key stops mattering.
  s.set_check_update_after_mod_install(false);
  probe.setValue(old_key, true);
  probe.sync();
  CHECK_FALSE(s.check_update_after_mod_install());

  // Neither key stored: the shipped default, ON.
  {
    probe.remove(old_key);
    probe.remove(new_key);
    probe.sync();
    CHECK(s.check_update_after_mod_install());
  }
}