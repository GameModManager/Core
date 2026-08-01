// Offscreen GUI test for the Plugins tab (Skyrim-style game plugins).
//
// Verifies:
//   - set_plugins() populates all four columns (name with in-cell enable
//     checkbox, flags, priority, mod index) in row order,
//   - force-loaded rows (game-native, CC) show a non-checkable checked box,
//     are greyed, and have no drag flag,
//   - the flags column renders MO2-style badge icons with the mark text in the
//     tooltip,
//   - missing-master rows are styled red italic with a tooltip naming the
//     missing master and the owning mod,
//   - toggling a checkbox emits toggle_requested(name, enabled),
//   - sync_enabled() updates checkboxes without re-emitting toggle_requested
//     (guards a blocked-toggle revert).
//
// Drag-reorder signal emission is NOT tested here: QApplication::notify
// routes Drop events to the active drag's current target only, so a
// synthesized QDropEvent never reaches dropEvent() and a real drag's modal
// QDrag::exec loop never completes on the offscreen platform. The engine-side
// move_plugin() is covered by plugin_database_test; the dropEvent wiring is
// verified manually.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, no network.
#include "ui/panels/tab_panels.h"

#include <QApplication>
#include <QTableWidget>
#include <QTableWidgetItem>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

static int failures = 0;
static int passes = 0;
static void check(bool cond, const char* what) {
    std::printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    if (cond)
        ++passes;
    else
        ++failures;
}

// Row whose Plugin Name column equals `name`, or -1.
static int row_with_name(QTableWidget* table, const char* name) {
    for (int r = 0; r < table->rowCount(); ++r) {
        auto* it = table->item(r, 0);
        if (it && it->text() == QLatin1String(name)) return r;
    }
    return -1;
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const std::filesystem::path cfg = "/tmp/gmm_plugins_tab/config";
    std::filesystem::remove_all("/tmp/gmm_plugins_tab");
    std::filesystem::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

    engine::GamePlugin native;  // game-native: pinned
    native.name = "Skyrim.esm";
    native.is_master = true;
    native.is_game_native = true;
    native.force_loaded = true;
    native.enabled = true;
    native.priority = 0;
    native.mod_index_text = "00";

    engine::GamePlugin cc;  // Creation Club: pinned
    cc.name = "ccBGSSSE001-Fish.esm";
    cc.is_cc = true;
    cc.force_loaded = true;
    cc.enabled = true;
    cc.priority = 1;
    cc.mod_index_text = "01";

    engine::GamePlugin skyui;  // user-band, light plugin
    skyui.name = "SkyUI_SE.esp";
    skyui.owner_mod = "SkyUI";
    skyui.masters = {"Skyrim.esm"};
    skyui.is_light = true;
    skyui.enabled = true;
    skyui.priority = 2;
    skyui.mod_index_text = "FE:000";

    engine::GamePlugin broken;  // user-band, missing master
    broken.name = "Broken.esp";
    broken.owner_mod = "Broken";
    broken.masters = {"Skyrim.esm", "GoneMaster.esm"};
    broken.missing_master = true;
    broken.enabled = false;
    broken.priority = 3;
    broken.mod_index_text = "02";

    const std::vector<engine::GamePlugin> plugins = {native, cc, skyui, broken};
    engine::GamePlugin broken_enabled = broken;
    broken_enabled.enabled = true;
    const std::vector<engine::GamePlugin> plugins_all_enabled = {
        native, cc, skyui, broken_enabled};

    ui::PluginsTab tab;
    auto* table = tab.table();
    tab.set_plugins(plugins);

    check(table->rowCount() == 4, "four plugin rows");
    check(table->columnCount() == 4, "four columns");
    check(row_with_name(table, "Skyrim.esm") == 0, "native ESM first");
    check(row_with_name(table, "ccBGSSSE001-Fish.esm") == 1, "CC second");
    check(row_with_name(table, "SkyUI_SE.esp") == 2, "mod plugin after CC");
    check(row_with_name(table, "Broken.esp") == 3, "broken plugin last");

    // Flags column: MO2-style badge icons, mark text in the tooltip.
    check(!table->item(0, 1)->icon().isNull() &&
              table->item(0, 1)->toolTip() == "ESM Native",
          "native flags icon + tooltip");
    check(!table->item(1, 1)->icon().isNull() &&
              table->item(1, 1)->toolTip() == "CC",
          "CC flags icon + tooltip");
    check(!table->item(2, 1)->icon().isNull() &&
              table->item(2, 1)->toolTip() == "ESL",
          "light flags icon + tooltip");
    check(table->item(3, 1)->icon().isNull(),
          "no flags icon for plain esp");
    check(table->item(0, 2)->text() == "0" && table->item(3, 2)->text() == "3",
          "priority column");
    check(table->item(2, 3)->text() == "FE:000" && table->item(3, 3)->text() == "02",
          "mod index column");

    // Pinned rows: checked, not user-checkable, greyed, not draggable.
    {
        QTableWidgetItem* en = table->item(0, 0);
        check(en->checkState() == Qt::Checked, "native shows checked");
        check(!(en->flags() & Qt::ItemIsUserCheckable), "native box not toggleable");
        check(!(en->flags() & Qt::ItemIsDragEnabled), "native row not draggable");
        check(table->item(0, 0)->foreground().color() == Qt::gray,
              "native name greyed");
        check(table->item(1, 0)->checkState() == Qt::Checked &&
                  !(table->item(1, 0)->flags() & Qt::ItemIsUserCheckable),
              "CC box pinned too");
    }

    // User-band rows are checkable and draggable.
    check(table->item(2, 0)->flags() & Qt::ItemIsUserCheckable,
          "mod plugin box toggleable");
    check(table->item(2, 0)->flags() & Qt::ItemIsDragEnabled,
          "mod plugin row draggable");

    // Missing master: red italic + tooltip naming the missing master.
    {
        QTableWidgetItem* name = table->item(3, 0);
        check(name->foreground().color() == QColor(0xB0, 0x30, 0x30),
              "missing-master name red");
        check(name->font().italic(), "missing-master name italic");
        const QString tip = name->toolTip();
        check(tip.contains("GoneMaster.esm") && tip.contains("Broken") &&
                  tip.contains("Skyrim.esm"),
              "tooltip names owner and masters");
    }

    // Toggling a checkbox emits toggle_requested with the plugin name.
    std::vector<std::pair<std::string, bool>> toggles;
    QObject::connect(&tab, &ui::PluginsTab::toggle_requested,
        [&](const std::string& name, bool enabled) {
            toggles.emplace_back(name, enabled);
        });
    table->item(3, 0)->setCheckState(Qt::Checked);
    QApplication::processEvents();
    check(toggles.size() == 1 && toggles[0].first == "Broken.esp" &&
              toggles[0].second,
          "checking the box emits toggle_requested");

    // sync_enabled() updates the boxes without re-emitting (guards a blocked
    // toggle revert - the engine rejects and the UI just refreshes).
    toggles.clear();
    tab.sync_enabled(plugins_all_enabled);
    QApplication::processEvents();
    check(toggles.empty(), "sync_enabled does not emit toggle_requested");
    check(table->item(3, 0)->checkState() == Qt::Checked,
          "sync_enabled reflects the new state");
    tab.sync_enabled(plugins);  // revert
    check(table->item(3, 0)->checkState() == Qt::Unchecked,
          "sync_enabled reverts the box");

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
