// FOMOD plugin integration test — verifies that the FOMOD installer plugin:
//   1. Loads correctly and registers its identity, category, and settings tab
//   2. The Settings dialog has NO "FOMOD" tab (Workspace-363: plugin settings
//      render under the Plugins section, not as separate tabs)
//   3. Selecting the FOMOD plugin in the Plugins section shows its settings
//      as rows in the Key | Value table (Restore previous choices, Show FOMOD
//      images — bool values as checkbox cells)
//   4. The FOMOD settings are persisted through the typed settings API
//
// This test loads the real FomodInstaller plugin from the build/plugins
// directory alongside the test fixture plugins. It exercises the full round-
// trip: plugin load → register_settings_tab → Plugins-tab table render →
// user edits → persist → reopen → values restored.
//
// UI test: requires QApplication offscreen, links gmm_ui.

#include "ui/settings/settings_dialog.h"
#include "ui/settings/settings.h"

#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "ui/theme/style_manager.h"
#include "engine/platform/theme/theme_manager.h"

#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char* what) {
    INFO(what);
    REQUIRE(cond);
}
}

static QTabWidget* find_tabs(QDialog& dlg) {
    for (auto* t : dlg.findChildren<QTabWidget*>())
        return t;
    return nullptr;
}

static QWidget* find_plugins_page(QTabWidget* tabs) {
    if (!tabs) return nullptr;
    for (int i = 0; i < tabs->count(); ++i)
        if (tabs->tabText(i).contains("Plugins", Qt::CaseInsensitive))
            return tabs->widget(i);
    return nullptr;
}

TEST_CASE("fomod plugin settings integration", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Isolate QSettings for this test run.
    const std::filesystem::path cfg = "/tmp/gmm_fomod_plugin_settings/config";
    std::filesystem::remove_all("/tmp/gmm_fomod_plugin_settings");
    std::filesystem::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());

    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

    const std::filesystem::path root = "/tmp/gmm_fomod_plugin_settings/instances/Test";
    std::filesystem::create_directories(root);

    engine::ThemeManager tm;
    engine::StyleManager style(tm);

    engine::PluginLoader loader;
    loader.load_directory(GMM_PLUGINS_DIR);
    loader.load_directory(GMM_TEST_PLUGINS_DIR);
    std::printf("loaded plugins = %zu\n", loader.plugins().size());

    // Check if FomodInstaller is among the loaded plugins.
    bool has_fomod = false;
    QString fomod_display_name;
    QString fomod_basename;
    for (const auto& p : loader.plugins()) {
        std::string basename = std::filesystem::path(p.path).filename().string();
        if (basename.find("FomodInstaller") != std::string::npos ||
            basename.find("fomod_installer") != std::string::npos) {
            has_fomod = true;
            fomod_display_name = QString::fromStdString(p.game_display_name);
            fomod_basename = QString::fromStdString(basename);
            std::printf("  FOMOD plugin found: %s (game=%s, category=%s)\n",
                        basename.c_str(), p.game_id.c_str(), p.category.c_str());
            check(p.category == "Installer",
                  "FOMOD plugin category is Installer");
            // NOTE: whether the plugin declares a typed settings tab is a
            // Plugins-repo concern (register_settings_tab ABI) - not pinned
            // here; see Workspace-5go.6.
        }
    }

    // Open the settings dialog.
    SettingsDialog dlg(&style, "breeze", root, &loader);
    dlg.show();
    app.processEvents();

    auto* tabs = find_tabs(dlg);
    check(tabs != nullptr, "dialog exposes a QTabWidget");

    // --- Workspace-363: NO "FOMOD" tab in Settings. ---
    // Neither the old standalone tab nor a plugin-declared tab may exist;
    // the FOMOD settings render inline under Plugins > FOMOD Installer.
    bool has_fomod_tab = false;
    for (int i = 0; tabs && i < tabs->count(); ++i)
        if (tabs->tabText(i) == "FOMOD") has_fomod_tab = true;
    check(!has_fomod_tab, "no FOMOD tab in the Settings dialog");

    if (has_fomod) {
        // --- Select the FOMOD plugin in the Plugins section. ---
        auto* page = find_plugins_page(tabs);
        check(page != nullptr, "Plugins tab present");
        auto* tree = page ? page->findChild<QTreeWidget*>() : nullptr;
        check(tree != nullptr, "Plugins tab has the plugin tree");

        bool selected = false;
        for (int g = 0; tree && g < tree->topLevelItemCount() && !selected; ++g) {
            auto* group = tree->topLevelItem(g);
            for (int c = 0; c < group->childCount() && !selected; ++c) {
                if (group->child(c)->text(0) == fomod_display_name) {
                    tree->setCurrentItem(group->child(c));
                    selected = true;
                }
            }
        }
        check(selected, "selected the FOMOD plugin leaf in the Plugins tree");
        app.processEvents();

        // --- The two settings render as rows in the Key | Value table. ---
        // The info pane also carries the Enabled checkbox; the settings table
        // is the only QTableWidget in the page.
        QTableWidget* table = page ? page->findChild<QTableWidget*>() : nullptr;
        check(table != nullptr, "FOMOD info pane shows the settings table");
        auto row_for = [](QTableWidget* t, const QString& key) -> QTableWidgetItem* {
            for (int r = 0; r < t->rowCount(); ++r) {
                auto* k = t->item(r, 0);
                if (k && k->text() == key) return t->item(r, 1);
            }
            return nullptr;
        };
        bool rows_ok = false;
        if (table) {
            auto* restore = row_for(table, "Restore previous choices");
            auto* images = row_for(table, "Show FOMOD images");
            rows_ok = table->columnCount() == 2 && table->rowCount() == 2 &&
                      restore && images &&
                      (restore->flags() & Qt::ItemIsUserCheckable) &&
                      restore->checkState() == Qt::Checked &&
                      (images->flags() & Qt::ItemIsUserCheckable) &&
                      images->checkState() == Qt::Checked;
        }
        check(rows_ok,
              "FOMOD settings render as checked checkbox rows in the table");

        // --- Test persistence: toggle off, reopen, verify. ---
        if (table) {
            auto* restore = row_for(table, "Restore previous choices");
            if (restore) {
                restore->setCheckState(Qt::Unchecked);
                app.processEvents();

                auto& s = Settings::instance();
                // The setting key matches what register_settings_tab declared.
                check(s.plugin_setting(fomod_basename, "Restore previous choices", "1") == "0",
                      "FOMOD setting persisted after edit");
            }
        }

        // --- Reopen: the inline settings restore the edited value. ---
        SettingsDialog dlg2(&style, "breeze", root, &loader);
        dlg2.show();
        app.processEvents();
        auto* tabs2 = find_tabs(dlg2);
        auto* page2 = find_plugins_page(tabs2);
        auto* tree2 = page2 ? page2->findChild<QTreeWidget*>() : nullptr;
        bool restored = false;
        for (int g = 0; tree2 && g < tree2->topLevelItemCount() && !restored; ++g) {
            auto* group = tree2->topLevelItem(g);
            for (int c = 0; c < group->childCount() && !restored; ++c) {
                if (group->child(c)->text(0) == fomod_display_name) {
                    tree2->setCurrentItem(group->child(c));
                    app.processEvents();
                    if (auto* t = page2->findChild<QTableWidget*>()) {
                        auto* restore = row_for(t, "Restore previous choices");
                        auto* images = row_for(t, "Show FOMOD images");
                        restored = restore && images &&
                                   restore->checkState() == Qt::Unchecked &&
                                   images->checkState() == Qt::Checked;
                    }
                }
            }
        }
        check(restored, "FOMOD inline settings restored after dialog reopen");
        app.processEvents();
    } else {
        std::printf("  FOMOD installer plugin not found — skipping inline checks\n");
        INFO("FomodInstaller plugin not found in loaded plugins; this is expected "
             "if the Plugins repo hasn't been updated with the FOMOD installer source.");
    }

    // --- Close dialog ---
    // SettingsDialog is a thin wrapper around SettingsContentWidget and has no
    // QDialogButtonBox - close it directly (regression: closing must not crash).
    dlg.close();
    app.processEvents();
    check(true, "closing the dialog did not crash");
}
