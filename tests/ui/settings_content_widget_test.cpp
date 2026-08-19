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

#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "ui/theme/style_manager.h"
#include "engine/platform/theme/theme_manager.h"

#include <QApplication>
#include <QCheckBox>
#include <QSignalSpy>
#include <QTabWidget>

#include <filesystem>
#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char* what) {
    INFO(what);
    REQUIRE(cond);
}
}

TEST_CASE("settings content widget embeds in a tab container", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Keep QSettings writes fully out of the user's real config.
    const std::filesystem::path cfg = "/tmp/gmm_settings_content/config";
    std::filesystem::remove_all("/tmp/gmm_settings_content");
    std::filesystem::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
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
    auto* tabs = content.tab_widget();
    check(tabs != nullptr, "panel exposes its internal QTabWidget");
    check(tabs && tabs->count() == 8, "panel has all 8 sub-tabs");
    if (tabs) {
        const char* expected[] = {"General", "Theme", "Mod List", "Paths",
                                  "Sources", "Plugins", "Workarounds", "Diagnostics"};
        bool names_ok = true;
        for (int i = 0; i < tabs->count(); ++i)
            names_ok = names_ok && tabs->tabText(i) == QLatin1String(expected[i]);
        check(names_ok, "sub-tab titles match the settings dialog layout");
    }

    // --- Embedding: MainTabContainer + dynamic "Settings" tab. ---
    ui::MainTabContainer container;
    auto* main_page = new QWidget(&container);
    container.add_main_tab(main_page);
    check(container.count() == 1, "container starts with only the Main tab");

    auto* settings_page = new ui::SettingsContentWidget(&style, "breeze", root,
                                                        &loader, &container);
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
        check(removed_spy.at(0).at(0).value<QWidget*>() == settings_page,
              "view_tab_removed carries the settings panel");
    check(container.count() == 1, "only the Main tab remains");
    check(!container.has_tab("settings"), "settings key forgotten on removal");
    delete settings_page;

    // --- full_ui_mode_toggled fires from the General-tab checkbox. ---
    ui::SettingsContentWidget content2(&style, "breeze", root, &loader);
    QSignalSpy mode_spy(&content2, &ui::SettingsContentWidget::full_ui_mode_toggled);
    QCheckBox* full_ui_box = nullptr;
    for (auto* cb : content2.tab_widget()->widget(0)->findChildren<QCheckBox*>())
        if (cb->text() == "Enable full UI tab mode") full_ui_box = cb;
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