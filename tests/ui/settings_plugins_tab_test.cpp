// Regression test for the Settings -> Plugins tab.
//
// Reproduces the crash path that SIGSEGV'd in QLabel::setText via
// currentItemChanged -> rebuild_info (the info-pane lambda captured a
// reference to a stack-local entries vector; the first setCurrentRow(0)
// fired while the vector was alive, later row clicks read freed memory).
// Also verifies register_category grouping into the foldable category
// tree, the filter, the Enabled toggle, the Path / Steam App ID rows
// being gone, the 723x634 minimum dialog size with both Plugins-tab
// columns stretching to the window bottom, and the register_settings
// mechanism: plugin options rendered as a Key | Value table (bool-like
// values as a checkbox, others as plaintext), persisted on edit and read
// back on reopen. Source providers must NOT show a settings container in
// this tab (only the Sources tab) - verified with a registered fake
// provider.
//
// Hermetic: plugins are loaded from the argv[1] dir when given (real
// register_category / register_settings ABI roundtrip); if none load,
// synthetic PluginInfo entries are injected so the UI logic is still
// exercised.
#include "ui/settings/settings_dialog.h"
#include "ui/settings/settings.h"

#include "engine/core/instance/instance_utils.h"
#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "engine/source/source_provider.h"
#include "ui/theme/style_manager.h"
#include "engine/platform/theme/theme_manager.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLineEdit>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
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

// Minimal provider so the "provider entries never show a settings container
// in the Plugins tab" path is exercised without shipping code.
struct FakeProvider : engine::SourceProvider {
    std::string source_type() const override { return "fakesrc"; }
    bool fetch(const engine::Mod&, engine::PipelineContext&,
               const std::filesystem::path&) override {
        return false;
    }
    std::string display_name() const override { return "Fake Source"; }
};

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

static void seed_synthetic(engine::PluginLoader& loader) {
    using engine::PluginInfo;
    PluginInfo a;
    a.game_display_name = "Test Game A";
    a.game_id = "test_a";
    a.author = "Author A";
    a.version = "1.0";
    a.description = "desc A";
    a.category = "Game Support";
    a.abi_version = 1;
    a.path = "/synthetic/TestGameA.so";
    a.settings = {{"masterlist_url", ""}, {"auto_sort_on_load", "1"}};
    PluginInfo b;
    b.game_display_name = "Test Tool B";
    b.game_id = "test_b";
    b.category = "Tool";
    b.path = "/synthetic/TestToolB.so";
    PluginInfo c;
    c.game_display_name = "Oddball C";
    c.game_id = "test_c";
    c.category = "Something Else";  // -> Uncategorized fallback
    c.path = "/synthetic/OddballC.so";
    loader.add_loaded_plugin(std::move(a));
    loader.add_loaded_plugin(std::move(b));
    loader.add_loaded_plugin(std::move(c));
}

TEST_CASE("settings plugins tab", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Keep QSettings writes fully out of the user's real config. Wipe any
    // previous run's state so persisted option edits don't leak across runs.
    const std::filesystem::path cfg = "/tmp/gmm_plugins_tab/config";
    std::filesystem::remove_all("/tmp/gmm_plugins_tab");
    std::filesystem::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

    const std::filesystem::path root = "/tmp/gmm_plugins_tab/instances/Test";
    std::filesystem::create_directories(root);

    engine::ThemeManager tm;
    engine::StyleManager style(tm);

    engine::PluginLoader loader;
    // Load the real plugins dir and the build/test_plugins fixture dir
    // (GMM_PLUGINS_DIR / GMM_TEST_PLUGINS_DIR compile definitions).
    loader.load_directory(GMM_PLUGINS_DIR);
    loader.load_directory(GMM_TEST_PLUGINS_DIR);
    std::printf("loaded %d plugin dir(s), plugins = %zu\n",
                2, loader.plugins().size());
    if (loader.plugins().empty()) seed_synthetic(loader);
    for (const auto& p : loader.plugins())
        std::printf("  - %-40s game=%-28s category=%s\n",
                    std::filesystem::path(p.path).filename().string().c_str(),
                    p.game_id.c_str(), p.category.c_str());
    const auto plugin_count = static_cast<int>(loader.plugins().size());

    SettingsDialog dlg(&style, "breeze", root, &loader);
    dlg.show();
    app.processEvents();

    auto* tabs = find_tabs(dlg);
    check(tabs != nullptr, "dialog exposes a QTabWidget");
    auto* page = find_plugins_page(tabs);
    check(page != nullptr, "Plugins tab present");

    auto* tree = page ? page->findChild<QTreeWidget*>() : nullptr;
    check(tree != nullptr, "left pane is a QTreeWidget (grouped)");

    // --- Category grouping from register_category strings. ---
    bool has_game_support = false, has_tool = false, has_sources = false;
    int total_leaves = 0;
    for (int g = 0; g < tree->topLevelItemCount(); ++g) {
        auto* group = tree->topLevelItem(g);
        total_leaves += group->childCount();
        QFont f = group->font(0);
        if (group->text(0) == "Game Support" && f.bold()) has_game_support = true;
        if (group->text(0) == "Tool" && f.bold()) has_tool = true;
        if (group->text(0) == "Sources") has_sources = true;
    }
    check(has_game_support, "Game Support group present and bold");
    check(has_tool, "Tool group present and bold");
    check(has_sources == false, "no Sources group without providers");
    check(total_leaves == plugin_count, "every loaded plugin appears as a leaf");

    bool headers_unselectable = true;
    for (int g = 0; g < tree->topLevelItemCount(); ++g)
        if (tree->topLevelItem(g)->flags() & Qt::ItemIsSelectable) headers_unselectable = false;
    check(headers_unselectable, "group headers are non-selectable");

    // --- THE CRASH PATH: click through every leaf. ---
    for (int g = 0; g < tree->topLevelItemCount(); ++g) {
        auto* group = tree->topLevelItem(g);
        for (int c = 0; c < group->childCount(); ++c) {
            tree->setCurrentItem(group->child(c));  // currentItemChanged -> rebuild_info
            app.processEvents();
            for (auto* box : dlg.findChildren<QCheckBox*>()) {
                if (box->text() == "Enabled" && box->isEnabled() && box->isVisible()) {
                    box->toggle();
                    box->toggle();
                    break;
                }
            }
            app.processEvents();
        }
    }
    check(true, "selected every plugin row + toggled Enabled without crashing");

    tree->setCurrentItem(tree->topLevelItem(0));
    app.processEvents();
    check(true, "clicking a group header did not crash");

    // --- Minimum dialog geometry (H634 x W723). ---
    const QSize min_size = dlg.minimumSize();
    check(min_size.width() == 723 && min_size.height() == 634,
          "settings dialog enforces min 723x634");

    // --- Both columns stretch to the bottom of the window. ---
    // The Plugins page must be the CURRENT tab (hidden pages get no real
    // geometry), then the dialog is resized larger than its minimum.
    if (tabs && page) tabs->setCurrentWidget(page);
    dlg.resize(1000, 850);
    app.processEvents();
    auto* splitter = page->findChild<QSplitter*>();
    check(splitter != nullptr, "Plugins tab uses a splitter");
    if (splitter && splitter->widget(0) && splitter->widget(1)) {
        const int h = splitter->height();
        const bool tall = h >= 700;
        const bool full =
            splitter->widget(0)->height() == h && splitter->widget(1)->height() == h;
        check(tall, "splitter stretches to the window bottom (>=700 of 850)");
        check(full, "both columns fill the full window height");
        check(splitter->widget(0)->minimumWidth() >= 361,
              "left column min width is half the min window width (>=361)");
    } else {
        check(false, "splitter or its columns missing");
    }

    // --- register_settings: Key | Value table. ---
    // Scan leaves until one shows a table with options; also note whether a
    // plugin without options shows the plain "no settings" label.
    bool saw_options = false, saw_no_settings_label = false;
    for (int g = 0; g < tree->topLevelItemCount() && !saw_options; ++g) {
        auto* group = tree->topLevelItem(g);
        for (int c = 0; c < group->childCount() && !saw_options; ++c) {
            tree->setCurrentItem(group->child(c));
            app.processEvents();
            if (auto* t = page->findChild<QTableWidget*>()) {
                if (t->columnCount() == 2 && t->rowCount() > 0) {
                    saw_options = true;
                    for (auto* lbl : page->findChildren<QLabel*>())
                        if (lbl->text() == "This plugin exposes no settings.")
                            saw_no_settings_label = true;
                    break;
                }
            }
        }
    }
    check(saw_options, "plugin options shown in a Key | Value table");
    // Revisit a leaf without options (first group may be all-options).
    if (!saw_no_settings_label) {
        for (int g = 0; g < tree->topLevelItemCount() && !saw_no_settings_label; ++g) {
            auto* group = tree->topLevelItem(g);
            for (int c = 0; c < group->childCount() && !saw_no_settings_label; ++c) {
                tree->setCurrentItem(group->child(c));
                app.processEvents();
                for (auto* lbl : page->findChildren<QLabel*>())
                    if (lbl->text() == "This plugin exposes no settings.")
                        saw_no_settings_label = true;
            }
        }
    }
    check(saw_no_settings_label,
          "plugin without options shows the plain no-settings label");

    // Select the options leaf again and verify the table headers + rows.
    int opt_g = -1, opt_c = -1;
    for (int g = 0; g < tree->topLevelItemCount() && opt_g < 0; ++g) {
        auto* group = tree->topLevelItem(g);
        for (int c = 0; c < group->childCount() && opt_g < 0; ++c) {
            tree->setCurrentItem(group->child(c));
            app.processEvents();
            if (auto* t = page->findChild<QTableWidget*>())
                if (t->columnCount() == 2 && t->rowCount() > 0) { opt_g = g; opt_c = c; }
        }
    }
    check(opt_g >= 0, "re-selected a plugin that exposes options");

    bool headers_ok = false;
    if (auto* t = page->findChild<QTableWidget*>()) {
        const auto* k = t->horizontalHeaderItem(0);
        const auto* v = t->horizontalHeaderItem(1);
        headers_ok = k && v && k->text() == "Key" && v->text() == "Value";
    }
    check(headers_ok, "table has Key | Value column headers");

    QTableWidgetItem* opt_masterlist = nullptr;
    QTableWidgetItem* opt_auto = nullptr;
    if (auto* t = page->findChild<QTableWidget*>()) {
        for (int r = 0; r < t->rowCount(); ++r) {
            auto* k = t->item(r, 0);
            if (!k) continue;
            if (k->text() == "masterlist_url") opt_masterlist = t->item(r, 1);
            else if (k->text() == "auto_sort_on_load") opt_auto = t->item(r, 1);
        }
    }
    check(opt_masterlist != nullptr && opt_auto != nullptr,
          "options rendered as table rows");
    check(opt_masterlist && opt_masterlist->text().isEmpty() &&
              !(opt_masterlist->flags() & Qt::ItemIsUserCheckable) &&
              opt_auto && opt_auto->checkState() == Qt::Checked &&
              (opt_auto->flags() & Qt::ItemIsUserCheckable),
          "plaintext value is a non-checkable text cell, bool value is a checked checkbox");

    // No "Settings" group box wraps the table; it is a direct child of the
    // info list. And as the only stretch-1 item it reaches the pane bottom.
    bool has_settings_box = false;
    QWidget* info_pane_w = nullptr;
    if (auto* sp = page->findChild<QSplitter*>()) info_pane_w = sp->widget(1);
    if (info_pane_w)
        for (auto* gb : info_pane_w->findChildren<QGroupBox*>())
            if (gb->title() == "Settings") has_settings_box = true;
    check(!has_settings_box, "no 'Settings' group box wraps the options table");
    if (auto* t = page->findChild<QTableWidget*>()) {
        const int pane_h = info_pane_w ? info_pane_w->height() : 0;
        check(info_pane_w != nullptr && t->height() >= 300 && pane_h > 0 &&
                  t->height() >= pane_h / 2,
              "options table stretches to the info pane bottom");
    }

    // --- Persistence: checkbox toggle + text edit write back to Settings. ---
    REQUIRE(opt_g >= 0);
    const QString options_leaf_name =
        tree->topLevelItem(opt_g)->child(opt_c)->text(0);
    QString options_basename;
    for (const auto& p : loader.plugins())
        if (QString::fromStdString(p.game_display_name) == options_leaf_name)
            options_basename =
                QString::fromStdString(std::filesystem::path(p.path).filename().string());
    check(!options_basename.isEmpty(), "resolved options plugin basename");
    opt_auto->setCheckState(Qt::Unchecked);
    opt_masterlist->setText("http://example/masterlist.yaml");
    app.processEvents();
    check(Settings::instance().plugin_setting(options_basename, "auto_sort_on_load", "1") == "0",
          "checkbox toggle persisted (plugins/settings/<basename>/<key>)");
    check(Settings::instance().plugin_setting(options_basename, "masterlist_url", "") ==
              "http://example/masterlist.yaml",
          "plaintext edit persisted (plugins/settings/<basename>/<key>)");

    // --- P1.5: typed plugin settings tab (register_settings_tab). ---
    // The fixture declares a "Fixture Settings" tab: bool/int/string/choice
    // rendered as native widgets, edits persisted through the same per-plugin
    // store, and its declared keys stop rendering as raw key:value rows in
    // the info pane (the undeclared plain_legacy_key keeps showing).
    QWidget* fixture_page = nullptr;
    for (int i = 0; tabs && i < tabs->count(); ++i)
        if (tabs->tabText(i) == "Fixture Settings") fixture_page = tabs->widget(i);
    check(fixture_page != nullptr, "plugin settings tab appended with its declared title");
    if (!fixture_page) {
        FAIL("no 'Fixture Settings' tab in the dialog");
    } else {
        const auto checkboxes = fixture_page->findChildren<QCheckBox*>();
        const auto spins = fixture_page->findChildren<QSpinBox*>();
        // QSpinBox/QDoubleSpinBox embed a QLineEdit internally; filter those
        // out so the string setting is the only plain line edit found.
        const auto edits = [&fixture_page] {
            QList<QLineEdit*> out;
            for (auto* e : fixture_page->findChildren<QLineEdit*>())
                if (!qobject_cast<QAbstractSpinBox*>(e->parentWidget())) out << e;
            return out;
        }();
        const auto combos = fixture_page->findChildren<QComboBox*>();
        check(checkboxes.size() == 1 && checkboxes[0]->isChecked(),
              "bool setting renders as a checked checkbox");
        check(spins.size() == 1 && spins[0]->value() == 4 &&
                  spins[0]->minimum() == 1 && spins[0]->maximum() == 8,
              "int setting renders as a spinbox honoring min:max");
        check(edits.size() == 1 && edits[0]->text() == "mod_",
              "string setting renders as a line edit");
        check(combos.size() == 1 && combos[0]->count() == 3 &&
                  combos[0]->currentText() == "Full",
              "choice setting renders as a combo box");

        QString fixture_basename;
        for (const auto& p : loader.plugins())
            if (QString::fromStdString(p.game_display_name) == "Settings Tab Fixture")
                fixture_basename = QString::fromStdString(
                    std::filesystem::path(p.path).filename().string());
        check(!fixture_basename.isEmpty(), "resolved fixture plugin basename");
        if (!fixture_basename.isEmpty()) {
            auto& s = Settings::instance();
            if (checkboxes.size() == 1) {
                checkboxes[0]->setChecked(false);
                app.processEvents();
                check(s.plugin_setting(fixture_basename, "show_previews", "1") == "0",
                      "bool edit persisted");
            }
            if (spins.size() == 1) {
                spins[0]->setValue(6);
                app.processEvents();
                check(s.plugin_setting(fixture_basename, "max_threads", "4") == "6",
                      "int edit persisted");
            }
            if (edits.size() == 1) {
                edits[0]->setText("bundle_");
                app.processEvents();
                check(s.plugin_setting(fixture_basename, "mod_name_prefix", "mod_") == "bundle_",
                      "string edit persisted");
            }
            if (combos.size() == 1) {
                combos[0]->setCurrentText("Compact");
                app.processEvents();
                check(s.plugin_setting(fixture_basename, "install_mode", "Full") == "Compact",
                      "choice edit persisted");
            }
        }
    }

    // Select the fixture leaf: declared keys filtered from the kv table, the
    // undeclared plain key kept, and a hint points at the typed tab.
    for (int g = 0; tree && g < tree->topLevelItemCount(); ++g) {
        auto* group = tree->topLevelItem(g);
        for (int c = 0; c < group->childCount(); ++c)
            if (group->child(c)->text(0) == "Settings Tab Fixture")
                tree->setCurrentItem(group->child(c));
    }
    app.processEvents();
    bool tab_hint_shown = false;
    bool only_legacy_row = false;
    if (page) {
        for (auto* lbl : page->findChildren<QLabel*>())
            if (lbl->text().startsWith("Settings live on the")) tab_hint_shown = true;
        if (auto* t = page->findChild<QTableWidget*>()) {
            only_legacy_row = t->rowCount() == 1 && t->item(0, 0) &&
                              t->item(0, 0)->text() == "plain_legacy_key";
        }
    }
    check(tab_hint_shown, "fixture info pane hints at the typed settings tab");
    check(only_legacy_row, "declared keys filtered from kv table, undeclared key kept");

    // --- Filter ---
    auto* filter = page->findChild<QLineEdit*>("pluginFilter");
    check(filter != nullptr, "filter bar present");
    if (filter) {
        const QString match_text = tree->topLevelItem(0)->child(0)->text(0).left(5);
        filter->setText(match_text);
        app.processEvents();
        int visible = 0;
        for (int g = 0; g < tree->topLevelItemCount(); ++g) {
            auto* group = tree->topLevelItem(g);
            if (group->isHidden()) continue;
            for (int c = 0; c < group->childCount(); ++c)
                if (!group->child(c)->isHidden()) ++visible;
        }
        check(visible > 0, "filter shows matches");
        filter->setText("zzz-no-such-plugin");
        app.processEvents();
        int visible_after = 0;
        for (int g = 0; g < tree->topLevelItemCount(); ++g) {
            auto* group = tree->topLevelItem(g);
            if (group->isHidden()) continue;
            for (int c = 0; c < group->childCount(); ++c)
                if (!group->child(c)->isHidden()) ++visible_after;
        }
        check(visible_after == 0, "non-matching filter hides every leaf");
        filter->clear();
        app.processEvents();
    }

    // --- Fold/unfold + reselect ---
    auto* first_group = tree->topLevelItem(0);
    first_group->setExpanded(false);
    app.processEvents();
    first_group->setExpanded(true);
    app.processEvents();
    for (int c = 0; c < first_group->childCount(); ++c) {
        tree->setCurrentItem(first_group->child(c));
        app.processEvents();
    }
    check(true, "fold/unfold + reselect did not crash");

    // --- Path / Steam App ID rows removed; metadata rows kept. ---
    bool has_path_row = false, has_appid_row = false, has_author = false, has_abi = false;
    for (auto* lbl : dlg.findChildren<QLabel*>()) {
        if (lbl->text() == "Path") has_path_row = true;
        if (lbl->text() == "Steam App ID") has_appid_row = true;
        if (lbl->text() == "Author") has_author = true;
        if (lbl->text() == "ABI version") has_abi = true;
    }
    check(!has_path_row && !has_appid_row, "no Path / Steam App ID rows in info pane");
    check(has_author && has_abi, "Author / ABI version rows still shown");

    // --- Paths tab: per-folder override fields. ---
    auto find_tab_page = [](QTabWidget* tw, const char* needle) -> QWidget* {
        for (int i = 0; tw && i < tw->count(); ++i)
            if (tw->tabText(i).contains(needle, Qt::CaseInsensitive))
                return tw->widget(i);
        return nullptr;
    };
    auto* paths_page = find_tab_page(tabs, "Paths");
    check(paths_page != nullptr, "Paths tab present");

    QLineEdit* mods_edit = nullptr;
    QLineEdit* downloads_edit = nullptr;
    QLineEdit* cache_edit = nullptr;
    QLineEdit* profiles_edit = nullptr;
    QLineEdit* overwrite_edit = nullptr;
    if (paths_page) {
        for (auto* le : paths_page->findChildren<QLineEdit*>()) {
            const auto ph = le->placeholderText();
            if (ph == "$BASE_DIRECTORY/mods") mods_edit = le;
            else if (ph == "$BASE_DIRECTORY/downloads") downloads_edit = le;
            else if (ph == "$BASE_DIRECTORY/cache") cache_edit = le;
            else if (ph == "$BASE_DIRECTORY/profiles") profiles_edit = le;
            else if (ph == "$BASE_DIRECTORY/overwrite") overwrite_edit = le;
        }
    }
    check(mods_edit && downloads_edit && cache_edit && profiles_edit && overwrite_edit,
          "five per-folder override fields with $BASE_DIRECTORY placeholders");
    check(mods_edit && mods_edit->text().isEmpty(),
          "folder fields start empty when no override is set");

    const QString custom_mods = "/tmp/gmm_plugins_tab/custom/mods";
    if (mods_edit) {
        mods_edit->setText(custom_mods);
        QMetaObject::invokeMethod(mods_edit, "editingFinished", Qt::DirectConnection);
        app.processEvents();
    }
    engine::Instance inst = engine::Instance::from_root(root);
    inst.read_toml();
    check(inst.path_for(engine::InstanceKind::Mods) == custom_mods.toStdString(),
          "edited Mods field persisted to instance.toml override");
    check(inst.path_for(engine::InstanceKind::Downloads) == root / "downloads",
          "unset folders still default under the base directory");
    check(inst.path_for(engine::InstanceKind::Cache) == root / "cache" &&
              inst.path_for(engine::InstanceKind::Overwrite) == root / "overwrite",
          "default cache/overwrite untouched by a mods-only override");

    // --- Close ---
    auto* buttons = dlg.findChild<QDialogButtonBox*>();
    check(buttons != nullptr, "dialog has a button box");
    if (buttons) {
        auto* close_btn = buttons->button(QDialogButtonBox::Close);
        check(close_btn != nullptr, "button box has a Close button");
        if (close_btn) close_btn->click();
    }
    app.processEvents();
    check(true, "closing the dialog did not crash");

    // --- Provider entries + reopen with a registered provider. ---
    engine::SourceRegistry::instance().register_provider(std::make_unique<FakeProvider>());

    SettingsDialog dlg2(&style, "breeze", root, &loader);
    dlg2.show();
    app.processEvents();

    auto* tabs2 = find_tabs(dlg2);
    auto* page2 = find_plugins_page(tabs2);
    auto* tree2 = page2 ? page2->findChild<QTreeWidget*>() : nullptr;
    check(tree2 != nullptr, "second dialog shows the Plugins tree");

    bool has_sources2 = false;
    for (int g = 0; tree2 && g < tree2->topLevelItemCount(); ++g)
        if (tree2->topLevelItem(g)->text(0) == "Sources") has_sources2 = true;
    check(has_sources2, "Sources group appears once a provider is registered");

    // Select the provider leaf: NO settings container, only the Sources hint.
    bool selected_provider = false;
    for (int g = 0; tree2 && g < tree2->topLevelItemCount() && !selected_provider; ++g) {
        auto* group = tree2->topLevelItem(g);
        for (int c = 0; c < group->childCount() && !selected_provider; ++c) {
            if (group->text(0) == "Sources") {
                tree2->setCurrentItem(group->child(c));
                selected_provider = true;
            }
        }
    }
    app.processEvents();
    check(selected_provider, "selected the source provider leaf");
    bool provider_hint = false;
    if (page2)
        for (auto* lbl : page2->findChildren<QLabel*>())
            if (lbl->text() == "Source provider settings live on the Sources tab.")
                provider_hint = true;
    check(provider_hint, "provider entry shows the Sources-tab hint");
    check(page2 == nullptr || page2->findChild<QTableWidget*>() == nullptr,
          "provider entry shows NO settings table in Plugins tab");

    // Reopen persistence: the edited option values survived into the new dialog.
    bool persisted_value = false;
    for (int g = 0; tree2 && g < tree2->topLevelItemCount() && !persisted_value; ++g) {
        auto* group = tree2->topLevelItem(g);
        for (int c = 0; c < group->childCount() && !persisted_value; ++c) {
            if (group->child(c)->text(0) == options_leaf_name) {
                tree2->setCurrentItem(group->child(c));
                app.processEvents();
                bool auto_unchecked = false, url_persisted = false;
                if (auto* t = page2->findChild<QTableWidget*>()) {
                    for (int r = 0; r < t->rowCount(); ++r) {
                        auto* k = t->item(r, 0);
                        if (!k) continue;
                        if (k->text() == "auto_sort_on_load" && t->item(r, 1))
                            auto_unchecked = t->item(r, 1)->checkState() == Qt::Unchecked;
                        else if (k->text() == "masterlist_url" && t->item(r, 1))
                            url_persisted = t->item(r, 1)->text() == "http://example/masterlist.yaml";
                    }
                }
                if (auto_unchecked && url_persisted)
                    persisted_value = true;
            }
        }
    }
    check(persisted_value,
          "edited option values persisted across dialog reopen");

    // --- Paths-tab override survives a dialog reopen. ---
    bool mods_override_shown = false;
    auto* paths2 = find_tab_page(tabs2, "Paths");
    if (paths2) {
        for (auto* le : paths2->findChildren<QLineEdit*>())
            if (le->placeholderText() == "$BASE_DIRECTORY/mods" &&
                le->text() == custom_mods)
                mods_override_shown = true;
    }
    check(mods_override_shown, "folder override shown again after dialog reopen");

    if (auto* buttons2 = dlg2.findChild<QDialogButtonBox*>())
        if (auto* close_btn = buttons2->button(QDialogButtonBox::Close))
            close_btn->click();
    app.processEvents();

    // --- P1.5 reopen: the edited typed-tab values survive into a new dialog.
    SettingsDialog dlg3(&style, "breeze", root, &loader);
    dlg3.show();
    app.processEvents();
    auto* tabs3 = find_tabs(dlg3);
    QWidget* fixture_page3 = nullptr;
    for (int i = 0; tabs3 && i < tabs3->count(); ++i)
        if (tabs3->tabText(i) == "Fixture Settings") fixture_page3 = tabs3->widget(i);
    bool typed_restored = false;
    if (fixture_page3) {
        const auto c3 = fixture_page3->findChildren<QCheckBox*>();
        const auto s3 = fixture_page3->findChildren<QSpinBox*>();
        QList<QLineEdit*> e3;
        for (auto* e : fixture_page3->findChildren<QLineEdit*>())
            if (!qobject_cast<QAbstractSpinBox*>(e->parentWidget())) e3 << e;
        const auto m3 = fixture_page3->findChildren<QComboBox*>();
        typed_restored = !c3.empty() && !c3[0]->isChecked() &&
                         !s3.empty() && s3[0]->value() == 6 &&
                         !e3.empty() && e3[0]->text() == "bundle_" &&
                         !m3.empty() && m3[0]->currentText() == "Compact";
    }
    check(typed_restored, "typed tab values persisted across dialog reopen");

    if (auto* buttons3 = dlg3.findChild<QDialogButtonBox*>())
        if (auto* close_btn = buttons3->button(QDialogButtonBox::Close))
            close_btn->click();
    app.processEvents();
}
