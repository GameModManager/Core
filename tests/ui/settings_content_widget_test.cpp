// Regression test for SettingsContentWidget (Workspace-3v5.4).
//
// Verifies the mode-agnostic settings panel extracted from SettingsDialog:
// the internal QTabWidget exposes all 8 sub-tabs (General, Theme, Mod List,
// Paths, Sources, Plugins, Workarounds, Diagnostics), the panel embeds as a
// dynamic tab inside MainTabContainer (Full UI tab mode), the tab can be
// selected/removed, and the full_ui_mode_toggled signal fires when the
// General-tab checkbox is toggled.
#include "ui/settings/settings_content_widget.h"
#include "ui/settings/settings.h"
#include "ui/widgets/main_tab_container.h"

#include "engine/parallel/parallel.h"
#include "engine/plugin_host/plugin_loader.h"
#include "ui/theme/style_manager.h"
#include "engine/platform/theme/theme_manager.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QSignalSpy>
#include <QTabWidget>

#include <filesystem>
#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

// The nine toggles in the General group box, in the order build_general_tab
// constructs them. The grid fills row-major, so construction order is also
// left-to-right, top-to-bottom order - which is what makes Tab order read the
// same way the boxes are laid out.
const char *const kGeneralToggles[] = {
    "Check for app updates every 24 hours",
    "Use prerelease updates",
    "Smooth scrolling in lists",
    "Show download notifications",
    "Close to system tray instead of quitting",
    "Enable full UI tab mode",
    "Lower priority during extraction",
    "Enable multi-core processing",
    "Double-click opens previews",
    "Confirm before switching game instance",
};
constexpr int kGeneralToggleCount =
    static_cast<int>(sizeof(kGeneralToggles) / sizeof(kGeneralToggles[0]));

// A General toggle is only usable if its Settings value survives the round
// trip, so pin each one to the accessor that seeds its default state.
using SettingGetter = bool (Settings::*)() const;
const QHash<QString, SettingGetter> kGeneralDefaults{
    {"Check for app updates every 24 hours", &Settings::check_for_updates},
    {"Use prerelease updates", &Settings::use_prereleases},
    {"Smooth scrolling in lists", &Settings::smooth_scrolling},
    {"Show download notifications", &Settings::show_download_notifications},
    {"Close to system tray instead of quitting", &Settings::minimize_to_tray},
    {"Enable full UI tab mode", &Settings::full_ui_mode},
    {"Lower priority during extraction", &Settings::extraction_low_priority},
    {"Enable multi-core processing", &Settings::performance_multi_core},
    {"Double-click opens previews", &Settings::double_clicks_open_previews},
    {"Confirm before switching game instance",
     &Settings::show_change_game_confirmation},
};

// The panel's ctor probes the update route and the General tab keys off the
// instance root's folder name, so both need a real StyleManager, PluginLoader
// and on-disk root rather than nulls. Rooted in the system temp dir and
// removed on scope exit.
struct Harness {
  engine::ThemeManager theme;
  engine::StyleManager style{theme};
  engine::PluginLoader loader;
  QDir root;

  Harness() : root(std::filesystem::temp_directory_path() / "gmm_general_two_column") {
    QDir().mkpath(root.filePath("instances/Test"));
  }
  ~Harness() { root.removeRecursively(); }

  std::filesystem::path instance_root() const {
    return root.filePath("instances/Test").toStdString();
  }
};
}  // namespace

TEST_CASE("settings content widget embeds in a tab container", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  // Keep QSettings writes fully out of the user's real config.
  const std::filesystem::path cfg = "/tmp/gmm_settings_content/config";
  std::filesystem::remove_all("/tmp/gmm_settings_content");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const std::filesystem::path root = "/tmp/gmm_settings_content/instances/Test";
  std::filesystem::create_directories(root);

  engine::ThemeManager tm;
  engine::StyleManager style(tm);
  engine::PluginLoader loader;

  // --- The panel exposes all 8 sub-tabs. ---
  ui::SettingsContentWidget content(&style, "breeze", root, &loader);
  auto *tabs = content.tab_widget();
  check(tabs != nullptr, "panel exposes its internal QTabWidget");
  check(tabs && tabs->count() == 8, "panel has all 8 sub-tabs");
  if (tabs) {
    const char *expected[] = {"General", "Theme",   "Mod List",    "Paths",
                              "Sources", "Plugins", "Workarounds", "Diagnostics"};
    bool names_ok          = true;
    for (int i = 0; i < tabs->count(); ++i)
      names_ok = names_ok && tabs->tabText(i) == QLatin1String(expected[i]);
    check(names_ok, "sub-tab titles match the settings dialog layout");
  }

  // --- Embedding: MainTabContainer + dynamic "Settings" tab. ---
  ui::MainTabContainer container;
  auto *main_page = new QWidget(&container);
  container.add_main_tab(main_page);
  check(container.count() == 1, "container starts with only the Main tab");

  auto *settings_page =
      new ui::SettingsContentWidget(&style, "breeze", root, &loader, &container);
  const int idx = container.add_view_tab(settings_page, "Settings", "settings");
  check(idx == 1 && container.count() == 2, "settings panel added as tab 1");
  check(container.currentWidget() == settings_page,
        "settings tab becomes the current tab");
  check(container.has_tab("settings"), "settings tab registered under its key");

  // Re-opening with the same key selects the existing tab, no duplicate.
  const int idx2 = container.add_view_tab(settings_page, "Settings", "settings");
  check(idx2 == idx && container.count() == 2,
        "re-opening the settings tab selects instead of duplicating");

  // --- Removing the tab emits view_tab_removed with the panel. ---
  QSignalSpy removed_spy(&container, &ui::MainTabContainer::view_tab_removed);
  container.remove_view_tab("settings");
  check(removed_spy.count() == 1, "removing the settings tab emits view_tab_removed");
  if (removed_spy.count() == 1)
    check(removed_spy.at(0).at(0).value<QWidget *>() == settings_page,
          "view_tab_removed carries the settings panel");
  check(container.count() == 1, "only the Main tab remains");
  check(!container.has_tab("settings"), "settings key forgotten on removal");
  delete settings_page;

  // --- full_ui_mode_toggled fires from the General-tab checkbox. ---
  ui::SettingsContentWidget content2(&style, "breeze", root, &loader);
  QSignalSpy mode_spy(&content2, &ui::SettingsContentWidget::full_ui_mode_toggled);
  QCheckBox *full_ui_box = nullptr;
  for (auto *cb : content2.tab_widget()->widget(0)->findChildren<QCheckBox *>())
    if (cb->text() == "Enable full UI tab mode")
      full_ui_box = cb;
  check(full_ui_box != nullptr, "General tab has the full-UI checkbox");
  if (full_ui_box) {
    const bool was_checked = full_ui_box->isChecked();
    full_ui_box->setChecked(!was_checked);
    app.processEvents();
    check(mode_spy.count() == 1, "toggling the checkbox emits full_ui_mode_toggled");
    if (mode_spy.count() == 1)
      check(mode_spy.at(0).at(0).toBool() == !was_checked,
            "signal carries the new mode value");
    full_ui_box->setChecked(was_checked);  // restore the setting
    app.processEvents();
  }
}

// The General tab stacks its nine toggles in a two-column grid rather than one
// long column. Two things have to survive that, and neither is visible in a
// screenshot: every toggle must still be IN the layout (a checkbox built but
// never added renders nowhere and is unreachable), and every toggle must still
// be CONNECTED (a checkbox that renders but no longer saves is a dead
// control). This asserts the whole inventory - count, labels, cells, enabled
// state, tooltips, defaults - rather than one representative box.
TEST_CASE("general tab toggles survive the two-column grid", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  Harness h;
  ui::SettingsContentWidget content(&h.style, "breeze", h.instance_root(), &h.loader);

  QGroupBox *gen_group = nullptr;
  for (auto *gb : content.tab_widget()->widget(0)->findChildren<QGroupBox *>())
    if (gb->title() == "General")
      gen_group = gb;
  check(gen_group != nullptr, "General tab still has its 'General' group box");
  if (!gen_group)
    return;

  auto *grid = qobject_cast<QGridLayout *>(gen_group->layout());
  check(grid != nullptr, "the group holds a grid, not a stacked box layout");
  if (!grid)
    return;
  check(grid->columnCount() == 2, "the grid is exactly two columns wide");

  QHash<QString, QCheckBox *> by_text;
  for (auto *cb : gen_group->findChildren<QCheckBox *>())
    by_text.insert(cb->text(), cb);
  check(by_text.size() == kGeneralToggleCount,
        "nine distinct toggles, none dropped or duplicated");
  REQUIRE(by_text.size() == kGeneralToggleCount);

  auto &s = Settings::instance();
  for (int i = 0; i < kGeneralToggleCount; ++i) {
    INFO(kGeneralToggles[i]);
    const QString text = QLatin1String(kGeneralToggles[i]);
    REQUIRE(by_text.contains(text));
    QCheckBox *cb = by_text.value(text);

    // In the layout at all: this is what "reachable" means here, and it is
    // the failure a build-time drop produces.
    const int idx = grid->indexOf(cb);
    check(idx != -1, "toggle is in the grid, so it has a position on screen");

    int row = -1, col = -1, rows = 0, cols = 0;
    grid->getItemPosition(idx, &row, &col, &rows, &cols);
    check(rows == 1 && cols == 1, "toggle occupies a single cell");
    // Rows 0 and 1 are the two full-width read-only labels; the toggles start
    // at row 2 and fill row-major, which is reading order and Tab order.
    check(row == 2 + i / 2 && col == i % 2,
          "toggle sits in the row-major cell that keeps left-to-right Tab order");

    check(cb->isChecked() == (s.*(kGeneralDefaults.value(text)))(),
          "default state still mirrors its Settings value");

    // "Show download notifications" is the one toggle that never carried a
    // tooltip; the other eight must all keep theirs.
    const bool wants_tip = text != "Show download notifications";
    if (wants_tip)
      check(!cb->toolTip().isEmpty(), "tooltip survived the layout change");
    else
      check(cb->toolTip().isEmpty(), "still the one toggle without a tooltip");
  }

  // "Use prerelease updates" is deliberately disabled: there is no channel
  // concept for it to select, and enabled it would advertise a choice that
  // does not exist. Disabled state and the tooltip saying why must both
  // survive the move into the grid.
  QCheckBox *prerelease = by_text.value("Use prerelease updates");
  check(prerelease && !prerelease->isEnabled(),
        "the prerelease toggle is still disabled in the grid");
  check(prerelease && !prerelease->toolTip().isEmpty(),
        "the prerelease toggle keeps the tooltip explaining why");

  // Narrow-window guard. A grid's minimum width is the SUM of its columns, so
  // if that sum ever exceeds the settings window's own minimum the page cannot
  // fit without clipping. The word-wrapped labels above the toggles span both
  // columns and wrap, so they do not contribute a wide minimum.
  INFO("grid min width: " << grid->totalMinimumSize().width() << " vs window min "
                          << ui::kSettingsMinWidth);
  check(grid->totalMinimumSize().width() <= ui::kSettingsMinWidth,
        "two columns fit within the settings window's minimum width");
}

// The multi-core toggle is the one General control with a consumer outside
// Settings: it pushes into engine::parallel, which is what the scanner reads to
// decide whether to fan out across cores. Asserting the stored key alone would
// pass with the engine push emptied out, so this checks the consumer too.
TEST_CASE("general grid toggles still write through to their consumer", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  Harness h;
  ui::SettingsContentWidget content(&h.style, "breeze", h.instance_root(), &h.loader);

  QCheckBox *multicore = nullptr;
  for (auto *cb : content.tab_widget()->widget(0)->findChildren<QCheckBox *>())
    if (cb->text() == "Enable multi-core processing")
      multicore = cb;
  check(multicore != nullptr, "the multi-core toggle is still on the General tab");
  if (!multicore)
    return;

  const bool before = multicore->isChecked();
  check(engine::parallel::enabled() == before,
        "the engine flag starts seeded from the stored setting");

  multicore->setChecked(!before);
  app.processEvents();
  check(Settings::instance().performance_multi_core() == !before,
        "toggling writes the settings key");
  check(engine::parallel::enabled() == !before,
        "toggling reaches the engine consumer, not just the store");

  multicore->setChecked(before);  // restore
  app.processEvents();
  check(engine::parallel::enabled() == before,
        "restoring the toggle restores the flag");
}