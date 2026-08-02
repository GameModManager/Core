// Offscreen GUI test for the Plugins tab (Skyrim-style game plugins).
//
// Verifies:
//   - set_plugins() populates all four columns (name with in-cell enable
//     checkbox, flags, priority, mod index) in row order,
//   - force-loaded rows (game-native, CC) show a non-checkable checked box,
//     are greyed, and have no drag flag,
//   - the flags column renders MO2-style status emblems (warning for missing
//     master, awaiting for ESL-flagged-without-.esl) with a reason tooltip,
//   - the plugin name carries the MO2 type font (bold master, italic light),
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
#include "ui/settings/settings.h"

#include <QApplication>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTest>

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
    native.is_master_flagged = true;
    native.has_master_ext = true;
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

    engine::GamePlugin skyui;  // user-band, ESL-flagged but .esp ext ("awaiting")
    skyui.name = "SkyUI_SE.esp";
    skyui.owner_mod = "SkyUI";
    skyui.masters = {"Skyrim.esm"};
    skyui.is_light_flagged = true;
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

    // Flags column: MO2-style status emblems — warning (missing master),
    // awaiting (ESL-flagged without .esl ext), run (medium/ESH). Type is shown
    // by the name font, not icons.
    check(table->item(0, 1)->icon().isNull(), "no emblem for a plain master ESM");
    check(table->item(1, 1)->icon().isNull(), "no emblem for a CC plugin");
    check(!table->item(2, 1)->icon().isNull() &&
              table->item(2, 1)->toolTip().contains("light (ESL)"),
          "awaiting emblem for ESL-flagged .esp");
    check(!table->item(3, 1)->icon().isNull() &&
              table->item(3, 1)->toolTip().contains("required master"),
          "warning emblem for missing master");
    check(table->item(0, 2)->text() == "0" && table->item(3, 2)->text() == "3",
          "priority column");
    check(table->item(2, 3)->text() == "FE:000" && table->item(3, 3)->text() == "02",
          "mod index column");

    // MO2-style type font on the name: bold = master/light ext, italic = light.
    check(table->item(0, 0)->font().bold(), "master ESM name bold");
    check(table->item(2, 0)->font().italic() && !table->item(2, 0)->font().bold(),
          "light-flagged name italic, not bold");

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

    // --- Selection highlight (MO2 parity) ---
    {
        // Contained: plugins owned by the mod selected in the mod list.
        tab.set_contained_plugins({QStringLiteral("SkyUI_SE.esp")});
        check(table->item(2, 0)->background().color() ==
                  Settings::instance().plugin_list_contained(),
              "contained row tinted with plugin_list_contained");
        check(table->item(2, 1)->background().color() ==
                  Settings::instance().plugin_list_contained(),
              "contained tint spans columns");

        // Masters: of the plugin selected in the plugin list.
        tab.set_master_plugins({QStringLiteral("Skyrim.esm")});
        check(table->item(0, 0)->background().color() ==
                  Settings::instance().plugin_list_master(),
              "master row tinted with plugin_list_master");

        // Contained wins over master when a row is both (MO2 check order).
        tab.set_contained_plugins({QStringLiteral("Skyrim.esm")});
        check(table->item(0, 0)->background().color() ==
                  Settings::instance().plugin_list_contained(),
              "contained beats master for the same row");

        // set_plugins() rebuilds the rows; the highlights must survive.
        tab.set_plugins(plugins);
        check(table->item(0, 0)->background().color() ==
                  Settings::instance().plugin_list_contained(),
              "contained highlight survives set_plugins()");
        check(table->item(2, 0)->background().style() == Qt::NoBrush,
              "unhighlighted row stays untinted after set_plugins()");

        // Regression: switching the selected mod must clear the previously
        // owned plugins' tint (select mod A, then mod B -> A's plugins revert
        // instead of staying blue/green).
        tab.set_contained_plugins({QStringLiteral("SkyUI_SE.esp")});
        check(table->item(2, 0)->background().color() ==
                  Settings::instance().plugin_list_contained(),
              "contained set A tints its plugins");
        tab.set_contained_plugins({QStringLiteral("ccBGSSSE001-Fish.esm")});
        check(table->item(2, 0)->background().style() == Qt::NoBrush,
              "switching contained set clears the previous mod's plugins");
        check(table->item(1, 0)->background().color() ==
                  Settings::instance().plugin_list_contained(),
              "new contained set tints the new mod's plugins");

        // Clearing the master set reverts master-only rows to untinted.
        tab.set_master_plugins({QStringLiteral("Skyrim.esm")});
        check(table->item(0, 0)->background().color() ==
                  Settings::instance().plugin_list_master(),
              "master-only row tinted with plugin_list_master");
        tab.set_master_plugins({});
        check(table->item(0, 0)->background().style() == Qt::NoBrush,
              "cleared master set reverts master-only row");
    }

    // selected_plugin_names() reports the selected rows in row order.
    {
        table->clearSelection();
        table->selectRow(2);
        check(tab.selected_plugin_names() ==
                  QStringList({QStringLiteral("SkyUI_SE.esp")}),
              "selected_plugin_names for one row");
        table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table->clearSelection();
        auto* sm = table->selectionModel();
        sm->select(table->model()->index(0, 0),
                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
        sm->select(table->model()->index(1, 0),
                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
        const QStringList sel = tab.selected_plugin_names();
        check(sel.size() == 2 && sel[0] == QStringLiteral("Skyrim.esm") &&
                  sel[1] == QStringLiteral("ccBGSSSE001-Fish.esm"),
              "selected_plugin_names in row order");
        table->clearSelection();
        check(tab.selected_plugin_names().isEmpty(),
              "selected_plugin_names empty after clear");
    }

    // --- Re-click deselection (MO2-style) ---
    // A plain left click on an already-selected plugin row clears the
    // selection; clicking an unselected row selects it; clicking the enable
    // checkbox (column 0's check indicator) keeps the selection.
    {
        tab.resize(640, 400);
        tab.show();
        QApplication::processEvents();
        auto* viewport = table->viewport();

        table->clearSelection();
        table->selectRow(2);
        check(table->selectionModel()->selectedRows().size() == 1,
              "prereq: selected plugin before re-click");
        QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier,
                          table->visualItemRect(table->item(2, 1)).center());
        check(table->selectionModel()->selectedRows().isEmpty(),
              "clicking the selected plugin again deselects it");

        QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier,
                          table->visualItemRect(table->item(3, 1)).center());
        const auto sel = table->selectionModel()->selectedRows();
        check(sel.size() == 1 && sel[0].row() == 3,
              "clicking an unselected plugin selects it");

        // Checkbox clicks keep the selection (they toggle enable state, not
        // selection) — hit the indicator via the same style query the view uses.
        table->clearSelection();
        table->selectRow(0);
        const QRect cell = table->visualItemRect(table->item(0, 0));
        QStyleOptionViewItem opt;
        opt.initFrom(table);
        opt.rect = cell;
        opt.features |= QStyleOptionViewItem::HasCheckIndicator;
        const QPoint indicator = table->style()
            ->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt, table)
            .center();
        QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, indicator);
        check(table->selectionModel()->selectedRows().size() == 1,
              "checkbox click keeps the selection");
        tab.hide();
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
