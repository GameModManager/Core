// Regression test for the Settings -> Plugins tab.
//
// Reproduces the crash path that SIGSEGV'd in QLabel::setText via
// currentItemChanged -> rebuild_info (the info-pane lambda captured a
// reference to a stack-local entries vector; the first setCurrentRow(0)
// fired while the vector was alive, later row clicks read freed memory).
// Also verifies register_category grouping into the foldable category
// tree, the filter, the Enabled toggle, and that the Path / Steam App ID
// rows are gone from the info pane.
//
// Hermetic: plugins are loaded from the argv[1] dir when given (real
// register_category ABI roundtrip); if none load, synthetic PluginInfo
// entries are injected so the UI logic is still exercised.
#include "ui/settings/settings_dialog.h"

#include "engine/instance/instance_utils.h"
#include "engine/plugin_host/plugin_loader.h"
#include "engine/theme/style_manager.h"
#include "engine/theme/theme_manager.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <cstdio>
#include <filesystem>

static int failures = 0;
static int passes = 0;
static void check(bool cond, const char* what) {
    std::printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
    if (cond)
        ++passes;
    else
        ++failures;
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

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Keep QSettings writes fully out of the user's real config.
    const std::filesystem::path cfg = "/tmp/gmm_plugins_tab/config";
    std::filesystem::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

    const std::filesystem::path root = "/tmp/gmm_plugins_tab/instances/Test";
    std::filesystem::create_directories(root);

    engine::ThemeManager tm;
    engine::StyleManager style(tm);

    engine::PluginLoader loader;
    const bool have_real = argc > 1 && loader.load_directory(argv[1]);
    std::printf("load_directory(%s) = %d, plugins = %zu\n",
                have_real ? argv[1] : "(none)", (int)have_real, loader.plugins().size());
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
    if (!tree) return 1;

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

    // --- Filter ---
    auto* filter = page->findChild<QLineEdit*>();
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

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
