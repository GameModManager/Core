// Offscreen GUI test for the Plugins tab (Skyrim-style game plugins).
//
// Verifies:
//   - set_plugins() populates all five columns (name with in-cell enable
//     checkbox, flags, priority, mod index, locked) in row order,
//   - force-loaded rows (game-native, CC) show a non-checkable checked box,
//     are greyed, and have no drag flag; forceEnabled rows pin checked but
//     stay draggable; forceDisabled rows pin unchecked, darkRed, no drag,
//   - the flags column renders MO2-style status emblems in MO2 order
//     (warning for unset masters/LOOT findings, information for messages,
//     attachment for INI, archive, awaiting for ESL-flagged-without-.esl,
//     run for ESH plus a second warning when both flags are set, dummy,
//     edit-clear for LOOT dirty findings) with per-emblem hover text
//     (kPluginFlagTooltipsRole, shown only for the emblem under the
//     cursor by FlagsDelegate::helpEvent) while the cell itself also carries
//     the full row tooltip (MO2 column-independent data()),
//   - the rich row tooltip matches MO2 tooltipData: Origin ("Data" for
//     game-owned), force lines, versions, raw author/description truncated at
//     1024 chars, Missing/Enabled Masters from master_unset (enabled rows
//     only), archives/INI/type paragraphs, forceDisabled block, raw messages
//     and the exact LOOT bullet list,
//   - no red/italic missing-master row styling (MO2 communicates it through
//     tooltip + warning emblem only),
//   - the plugin name carries the MO2 type font (bold master, italic light),
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
#include "ui/theme/icon_manager.h"
#include "ui/widgets/mod_table_view.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QLCDNumber>
#include <QList>
#include <QMenu>
#include <QPoint>
#include <QPushButton>
#include <QRect>
#include <QStringList>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTest>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {
void check(bool cond, const char* what) {
  INFO(what);
  REQUIRE(cond);
}
}  // namespace

// Row whose Plugin Name column equals `name`, or -1.
static int row_with_name(QTableWidget* table, const char* name) {
  for (int r = 0; r < table->rowCount(); ++r) {
    auto* it = table->item(r, 0);
    if (it && it->text() == QLatin1String(name))
      return r;
  }
  return -1;
}

static QAction* action_with_text(QMenu& menu, const char* text) {
  for (auto* a : menu.actions()) {
    if (a->text() == QLatin1String(text))
      return a;
  }
  return nullptr;
}

// Expose the protected context-menu builder for direct driving (menu.exec() is
// modal, so the full on_custom_context_menu flow is not exercised).
struct TestPluginsTab : ui::PluginsTab {
  using ui::PluginsTab::add_context_menu_actions;
};

TEST_CASE("plugins tab", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_plugins_tab/config";
  std::filesystem::remove_all("/tmp/gmm_plugins_tab");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  // macOS QSettings NativeFormat ignores XDG_CONFIG_HOME (uses
  // ~/Library/Preferences plist); force IniFormat under the throwaway dir.
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char* test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // IconManager resolves icons from <arg>/../resources; CMake hands us the
  // source-tree resources/ dir so the pack chain is found in any build
  // layout (in-tree or out-of-tree). Without it the lock pin (base pack)
  // comes back null on the offscreen platform.
  engine::IconManager::instance().discover_packs(GMM_TEST_RESOURCES_DIR);

  engine::GamePlugin native;  // game-native: pinned
  native.name              = "Skyrim.esm";
  native.is_master_flagged = true;
  native.has_master_ext    = true;
  native.is_game_native    = true;
  native.force_loaded      = true;
  native.enabled           = true;
  native.priority          = 0;
  native.mod_index_text    = "00";

  engine::GamePlugin cc;  // Creation Club: pinned
  cc.name           = "ccBGSSSE001-Fish.esm";
  cc.is_cc          = true;
  cc.force_loaded   = true;
  cc.enabled        = true;
  cc.priority       = 1;
  cc.mod_index_text = "01";

  engine::GamePlugin skyui;  // user-band, ESL-flagged but .esp ext ("awaiting")
  skyui.name             = "SkyUI_SE.esp";
  skyui.owner_mod        = "SkyUI";
  skyui.masters          = {"Skyrim.esm"};
  skyui.is_light_flagged = true;
  skyui.enabled          = true;
  skyui.priority         = 2;
  skyui.mod_index_text   = "FE:000";

  engine::GamePlugin broken;  // user-band, missing master (disabled: MO2 shows
  broken.name            = "Broken.esp";  // no warning and no Missing Masters line for
  broken.owner_mod       = "Broken";  // disabled rows - only the Enabled Masters line
  broken.masters         = {"Skyrim.esm", "GoneMaster.esm"};  // lists every master.
  broken.missing_master  = true;
  broken.missing_masters = {"GoneMaster.esm"};
  broken.enabled         = false;
  broken.priority        = 3;
  broken.mod_index_text  = "";

  engine::GamePlugin needs_disabled;  // enabled, master present-but-disabled
  needs_disabled.name           = "NeedsDisabled.esp";  // (MO2 testMasters parity):
  needs_disabled.owner_mod      = "NeedsMod";  // warning emblem + Missing Masters
  needs_disabled.masters        = {"Skyrim.esm", "DisabledMaster.esm"};  // line, and
  needs_disabled.master_unset   = {"DisabledMaster.esm"};  // DisabledMaster is
  needs_disabled.enabled        = true;  // absent from Enabled Masters.
  needs_disabled.priority       = 4;
  needs_disabled.mod_index_text = "02";

  const std::vector<engine::GamePlugin> plugins = {native, cc, skyui, broken,
                                                   needs_disabled};
  engine::GamePlugin broken_enabled             = broken;
  broken_enabled.enabled                        = true;
  const std::vector<engine::GamePlugin> plugins_all_enabled = {
      native, cc, skyui, broken_enabled, needs_disabled};

  TestPluginsTab tab;
  auto* table = tab.table();
  tab.set_plugins(plugins);

  check(table->rowCount() == 5, "five plugin rows");
  check(table->columnCount() == 5, "five columns");
  check(table->horizontalHeaderItem(4)->text() == QLatin1String("Locked"),
        "Locked column header");
  check(row_with_name(table, "Skyrim.esm") == 0, "native ESM first");
  check(row_with_name(table, "ccBGSSSE001-Fish.esm") == 1, "CC second");
  check(row_with_name(table, "SkyUI_SE.esp") == 2, "mod plugin after CC");
  check(row_with_name(table, "Broken.esp") == 3, "broken plugin fourth");
  check(row_with_name(table, "NeedsDisabled.esp") == 4, "unset-master plugin fifth");

  // Flags column: MO2-style status emblems — warning (missing master),
  // awaiting (ESL-flagged without .esl ext), run (medium/ESH). Type is shown
  // by the name font, not icons. Emblems ride kPluginFlagsRole as individual
  // QIcons; the FlagsDelegate paints them one-by-one at native size (never
  // stacked into one pixmap that Qt would squeeze into a single icon slot).
  check(table->item(0, 1)->data(ui::PluginsTab::kPluginFlagsRole).isNull(),
        "no emblem for a plain master ESM");
  check(table->item(1, 1)->data(ui::PluginsTab::kPluginFlagsRole).isNull(),
        "no emblem for a CC plugin");
  // Per-flag hover text rides kPluginFlagTooltipsRole as a parallel
  // QStringList (a GMM additive extra answered by FlagsDelegate::helpEvent);
  // the cell itself also carries the full row tooltip, like MO2's
  // column-independent data().
  const QStringList skyui_tips = table->item(2, 1)
                                     ->data(ui::PluginsTab::kPluginFlagTooltipsRole)
                                     .value<QStringList>();
  const QStringList needs_tips = table->item(4, 1)
                                     ->data(ui::PluginsTab::kPluginFlagTooltipsRole)
                                     .value<QStringList>();
  check(skyui_tips.size() == 1 && skyui_tips[0].contains("light plugin (ESL)"),
        "awaiting emblem hover text");
  // MO2 testMasters parity: the disabled broken row (absent master) shows no
  // warning emblem; the enabled row with a present-but-disabled master does.
  check(table->item(3, 1)->data(ui::PluginsTab::kPluginFlagsRole).isNull(),
        "disabled row with absent master shows no warning emblem");
  check(needs_tips.size() == 1 && needs_tips[0].contains("Missing Masters") &&
            needs_tips[0].contains("DisabledMaster.esm"),
        "warning emblem hover text");
  check(!table->item(2, 1)->toolTip().isEmpty() &&
            table->item(2, 1)->toolTip() == table->item(2, 0)->toolTip() &&
            table->item(4, 1)->toolTip() == table->item(4, 0)->toolTip(),
        "flags cell shares the row tooltip (MO2 column-independent)");
  check(table->item(0, 2)->text() == "0" && table->item(3, 2)->text() == "3",
        "priority column");
  check(table->item(2, 3)->text() == "FE:000" && table->item(3, 3)->text().isEmpty() &&
            table->item(4, 3)->text() == "02",
        "mod index column (disabled rows stay empty)");

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
    check(table->item(0, 0)->foreground().color() == Qt::gray, "native name greyed");
    check(table->item(1, 0)->checkState() == Qt::Checked &&
              !(table->item(1, 0)->flags() & Qt::ItemIsUserCheckable),
          "CC box pinned too");
  }

  // User-band rows are checkable and draggable.
  check(table->item(2, 0)->flags() & Qt::ItemIsUserCheckable,
        "mod plugin box toggleable");
  check(table->item(2, 0)->flags() & Qt::ItemIsDragEnabled, "mod plugin row draggable");

  // MO2 parity: missing masters are communicated by tooltip + warning emblem
  // only - no red/italic row styling. The disabled broken row renders like a
  // plain row; its tooltip still lists every master under Enabled Masters.
  {
    QTableWidgetItem* name = table->item(3, 0);
    check(name->foreground().style() == Qt::NoBrush, "disabled broken row untinted");
    check(!name->font().italic(), "disabled broken row not italic");
    const QString tip = name->toolTip();
    check(!tip.contains("Missing Masters"), "no Missing Masters line when disabled");
    check(tip.contains("Enabled Masters") && tip.contains("GoneMaster.esm") &&
              tip.contains("Broken"),
          "disabled tooltip lists owner and every master as enabled");
  }

  // Enabled row with a present-but-disabled master: Missing Masters names it
  // (bold-wrapped, MO2 order), Enabled Masters omits it.
  {
    const QString tip = table->item(4, 0)->toolTip();
    check(tip.contains("<b>Missing Masters</b>: <b>DisabledMaster.esm</b>"),
          "Missing Masters line names the unset master");
    check(tip.contains("Enabled Masters") && tip.contains("Skyrim.esm") &&
              !tip.contains("Enabled Masters</b>: DisabledMaster") &&
              tip.indexOf("DisabledMaster.esm") < tip.indexOf("Enabled Masters"),
          "Enabled Masters omits the unset master");
  }

  // Toggling a checkbox emits toggle_requested with the plugin name.
  std::vector<std::pair<std::string, bool>> toggles;
  QObject::connect(&tab, &ui::PluginsTab::toggle_requested,
                   [&](const std::string& name, bool enabled) {
                     toggles.emplace_back(name, enabled);
                   });
  table->item(3, 0)->setCheckState(Qt::Checked);
  QApplication::processEvents();
  check(toggles.size() == 1 && toggles[0].first == "Broken.esp" && toggles[0].second,
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
    check(tab.selected_plugin_names() == QStringList({QStringLiteral("SkyUI_SE.esp")}),
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
    const QPoint indicator =
        table->style()
            ->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt, table)
            .center();
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, indicator);
    check(table->selectionModel()->selectedRows().size() == 1,
          "checkbox click keeps the selection");
    tab.hide();
  }

  // --- Rich HTML tooltip (MO2 tooltipData parity) ---
  {
    engine::GamePlugin rich;  // metadata-rich user plugin
    rich.name           = "RichPlugin.esp";
    rich.owner_mod      = "RichMod";
    rich.masters        = {"Skyrim.esm"};
    rich.enabled        = true;
    rich.priority       = 4;
    rich.mod_index_text = "03";
    rich.form_version   = 44;
    rich.header_version = 1.70f;
    rich.author         = "Test Author <tester>";
    rich.description    = "A rich test description";
    rich.has_ini        = true;
    rich.archives       = {"RichPlugin - Main.bsa", "RichPlugin - Textures.bsa"};
    rich.messages       = {"Message one", "Message two"};

    engine::GamePlugin dummy;  // no records, paired with an archive
    dummy.name           = "Dummy.esl";
    dummy.owner_mod      = "DummyMod";
    dummy.enabled        = true;
    dummy.has_no_records = true;
    dummy.has_light_ext  = true;
    dummy.priority       = 5;
    dummy.mod_index_text = "FE:001";

    engine::GamePlugin locked;  // user-pinned: immovable
    locked.name           = "Locked.esp";
    locked.owner_mod      = "LockedMod";
    locked.enabled        = true;
    locked.locked         = true;
    locked.priority       = 6;
    locked.mod_index_text = "04";

    const std::vector<engine::GamePlugin> rich_set = {native, cc,    skyui, broken,
                                                      rich,   dummy, locked};
    tab.set_plugins(rich_set);

    const int rr = row_with_name(table, "RichPlugin.esp");
    check(rr >= 0, "rich plugin row present");
    const QString rt = table->item(rr, 0)->toolTip();
    check(rt.contains("Origin") && rt.contains("RichMod"), "tooltip Origin line");
    check(rt.contains("Form Version") && rt.contains("44"),
          "tooltip Form Version line");
    check(rt.contains("Header Version") && rt.contains("1.7"),
          "tooltip Header Version line");
    check(rt.contains("Author") && rt.contains("Test Author"), "tooltip Author line");
    check(rt.contains("Description") && rt.contains("test description"),
          "tooltip Description line");
    check(rt.contains("Loads Archives") && rt.contains("Textures.bsa"),
          "tooltip Loads Archives line");
    check(rt.contains("Loads INI settings"), "tooltip Loads INI line");
    check(rt.contains("Message one") && rt.contains("Message two"),
          "tooltip diagnostics messages");
    check(!rt.contains("Missing Masters"), "no Missing Masters for a healthy plugin");
    // The same rich tooltip is shared by every column including Flags
    // (MO2 tooltipData is column-independent).
    check(table->item(rr, 0)->toolTip() == table->item(rr, 1)->toolTip() &&
              table->item(rr, 0)->toolTip() == table->item(rr, 2)->toolTip() &&
              table->item(rr, 0)->toolTip() == table->item(rr, 3)->toolTip(),
          "tooltip identical across all columns");
    // Per-flag hover fragments: the rich plugin's three emblems (information,
    // INI, BSA - MO2 iconData order) expose exactly their own fragment, not
    // the whole rich tooltip.
    const QStringList rich_tips = table->item(rr, 1)
                                      ->data(ui::PluginsTab::kPluginFlagTooltipsRole)
                                      .value<QStringList>();
    check(rich_tips.size() == 3, "per-flag tooltips parallel the emblems");
    check(rich_tips[0].contains("Message one") &&
              !rich_tips[0].contains("Loads INI settings") &&
              !rich_tips[0].contains("Origin"),
          "information emblem shows only the messages fragment");
    check(rich_tips[1].contains("Loads INI settings") &&
              !rich_tips[1].contains("Loads Archives") &&
              !rich_tips[1].contains("Origin"),
          "INI emblem shows only its own fragment");
    check(rich_tips[2].contains("Loads Archives") &&
              !rich_tips[2].contains("Loads INI settings"),
          "archive emblem shows only its own fragment");

    // Dummy plugin: emblem + explanatory paragraph.
    const int dr = row_with_name(table, "Dummy.esl");
    check(!table->item(dr, 1)
               ->data(ui::PluginsTab::kPluginFlagsRole)
               .value<QList<QIcon>>()
               .isEmpty(),
          "dummy emblem shown for a no-records plugin");
    check(table->item(dr, 0)->toolTip().contains("dummy plugin"),
          "tooltip dummy paragraph");

    // Locked plugin: immovable (no drag flag) but still toggleable, with
    // the lock pin in its own rightmost column (not a Flags emblem) and the
    // full tooltip intact.
    const int lr         = row_with_name(table, "Locked.esp");
    QTableWidgetItem* ln = table->item(lr, 0);
    check(ln->flags() & Qt::ItemIsUserCheckable, "locked row still toggleable");
    check(!(ln->flags() & Qt::ItemIsDragEnabled), "locked row not draggable");
    check(!(table->item(lr, 1)->flags() & Qt::ItemIsDragEnabled) &&
              !(table->item(lr, 2)->flags() & Qt::ItemIsDragEnabled) &&
              !(table->item(lr, 3)->flags() & Qt::ItemIsDragEnabled) &&
              !(table->item(lr, 4)->flags() & Qt::ItemIsDragEnabled),
          "locked row drag disabled on every column");
    check(table->item(lr, 1)->data(ui::PluginsTab::kPluginFlagsRole).isNull(),
          "lock is no longer a Flags emblem");
    check(!table->item(lr, 4)->icon().isNull(), "lock pin shown in the Locked column");
    check(table->item(lr, 4)->toolTip().contains("locked"),
          "Locked cell hover explains the pin");
    check(table->item(rr, 4)->icon().isNull(), "unlocked rich row has no lock pin");
    check(!ln->toolTip().isEmpty(), "locked row keeps the rich tooltip");

    // Multiple emblems on one row stay separate QIcons (the composite
    // pixmap approach would have squeezed them into a single slot).
    check(table->item(rr, 1)
                  ->data(ui::PluginsTab::kPluginFlagsRole)
                  .value<QList<QIcon>>()
                  .size() == 3,
          "multi-emblem row exposes each emblem as its own icon");

    // flag_icon_at() shares the wrap math with the delegate's paint, so a
    // hover hit-test can never disagree with what is drawn. A 40px-wide
    // cell fits two 16px icons (18px step): icon 0 at x 0-15, icon 1 at
    // x 18-33.
    {
      const QList<QIcon> two = table->item(rr, 1)
                                   ->data(ui::PluginsTab::kPluginFlagsRole)
                                   .value<QList<QIcon>>();
      const QRect cell(0, 0, 40, 16);
      check(ui::flag_icon_at(two, cell, QPoint(2, 8)) == 0,
            "flag_icon_at hits the first emblem");
      check(ui::flag_icon_at(two, cell, QPoint(20, 8)) == 1,
            "flag_icon_at hits the second emblem");
      check(ui::flag_icon_at(two, cell, QPoint(5, 20)) == -1,
            "flag_icon_at misses below the emblems");
      check(ui::flag_icon_at(two, cell, QPoint(35, 8)) == -1,
            "flag_icon_at misses on the right padding");
      check(ui::flag_icon_at({}, cell, QPoint(5, 8)) == -1,
            "flag_icon_at with an empty list misses");
    }

    // A plugin flagged both ESL and ESH shows the awaiting + run emblems plus
    // MO2's second warning icon (pluginlist.cpp:1752-1757); both type
    // fragments carry the both-flags warning.
    engine::GamePlugin both;
    both.name                                      = "Both.esp";
    both.owner_mod                                 = "BothMod";
    both.enabled                                   = true;
    both.is_light_flagged                          = true;
    both.is_medium_flagged                         = true;
    both.priority                                  = 7;
    both.mod_index_text                            = "FE:002";
    const std::vector<engine::GamePlugin> both_set = {native, cc,    skyui,  broken,
                                                      rich,   dummy, locked, both};
    tab.set_plugins(both_set);
    const int br                  = row_with_name(table, "Both.esp");
    const QList<QIcon> both_icons = table->item(br, 1)
                                        ->data(ui::PluginsTab::kPluginFlagsRole)
                                        .value<QList<QIcon>>();
    const QStringList both_tips   = table->item(br, 1)
                                        ->data(ui::PluginsTab::kPluginFlagTooltipsRole)
                                        .value<QStringList>();
    check(both_icons.size() == 3, "both-flags row shows three emblems");
    check(both_tips.size() == 3, "both-flags row shows three fragments");
    check(both_tips[0].contains("light plugin (ESL)") &&
              both_tips[0].contains("both light and medium flagged"),
          "awaiting fragment carries the both-flags warning");
    check(both_tips[1].contains("medium plugin (ESH)") &&
              both_tips[1].contains("both light and medium flagged"),
          "run fragment carries the both-flags warning");
    check(both_tips[2].contains("both light and medium flagged") &&
              !both_tips[2].contains("light plugin (ESL)") &&
              !both_tips[2].contains("medium plugin (ESH)"),
          "extra warning emblem carries only the both-flags warning");

    // --- MO2 force lines, truncation, origin, LOOT bullets ---
    {
      engine::GamePlugin force_on;  // game-enforced enabled (MO2 forceEnabled)
      force_on.name          = "ForcedOn.esp";
      force_on.owner_mod     = "ForceMod";
      force_on.force_enabled = true;
      force_on.enabled       = true;
      force_on.priority      = 8;

      engine::GamePlugin force_off_esl;  // .esl the game cannot load
      force_off_esl.name           = "ForcedOff.esl";
      force_off_esl.force_disabled = true;
      force_off_esl.has_light_ext  = true;
      force_off_esl.enabled        = false;
      force_off_esl.priority       = 9;

      engine::GamePlugin force_off;  // generic unloadable plugin
      force_off.name           = "ForcedOff.esp";
      force_off.force_disabled = true;
      force_off.enabled        = false;
      force_off.priority       = 10;

      engine::GamePlugin verbose;  // over-long free-text fields
      verbose.name        = "Verbose.esp";
      verbose.owner_mod   = "VerboseMod";
      verbose.enabled     = true;
      verbose.author      = std::string(1100, 'x');
      verbose.description = std::string(1100, 'y');
      verbose.priority    = 11;

      engine::GamePlugin looted;  // full LOOT report (MO2 makeLootTooltip)
      looted.name      = "Looted.esp";
      looted.owner_mod = "LootMod";
      looted.enabled   = true;
      looted.masters   = {"Skyrim.esm"};
      looted.priority  = 12;
      engine::LootReport rep;
      rep.incompatibilities.emplace_back("Other.esp", "Other Mod");
      rep.incompatibilities.emplace_back("Nameless.esp", "");
      rep.missing_masters.push_back("GoneMaster.esm");
      rep.messages.push_back({"warning", "A <a href=\"http://example.com\">note</a>"});
      rep.messages.push_back({"error", "Bad thing"});
      rep.messages.push_back({"info", "Plain"});
      rep.dirty.push_back({3, 1, 2, "SSEEdit", "extra"});
      rep.dirty.push_back({0, 0, 0, "", ""});
      rep.clean.push_back({"SSEEdit"});
      looted.loot_report = rep;

      tab.set_plugins({native, cc, skyui, broken, needs_disabled, force_on,
                       force_off_esl, force_off, verbose, looted});

      // forceEnabled: pinned checked box, "can't be disabled" line (without
      // "or moved"), draggable, normal foreground.
      {
        const int fr         = row_with_name(table, "ForcedOn.esp");
        QTableWidgetItem* fn = table->item(fr, 0);
        check(fn->checkState() == Qt::Checked, "forceEnabled shows checked");
        check(!(fn->flags() & Qt::ItemIsUserCheckable),
              "forceEnabled box not toggleable");
        check(fn->flags() & Qt::ItemIsDragEnabled, "forceEnabled row draggable");
        check(fn->foreground().style() == Qt::NoBrush, "forceEnabled name not greyed");
        check(table->item(fr, 0)->toolTip().contains(
                  "This plugin can't be disabled (enforced by the game)."),
              "forceEnabled tooltip line");
      }

      // forceDisabled: unchecked pinned box, darkRed name, no drag, and the
      // matching forceDisabled paragraph (.esl variant vs generic).
      {
        const int er         = row_with_name(table, "ForcedOff.esl");
        QTableWidgetItem* en = table->item(er, 0);
        check(en->checkState() == Qt::Unchecked, "forceDisabled shows unchecked");
        check(!(en->flags() & Qt::ItemIsUserCheckable),
              "forceDisabled box not toggleable");
        check(!(en->flags() & Qt::ItemIsDragEnabled),
              "forceDisabled row not draggable");
        check(table->item(er, 0)->foreground().color() == Qt::darkRed,
              "forceDisabled name darkRed");
        check(table->item(er, 0)->toolTip().contains(
                  "Light plugins (ESL) are not supported by this game."),
              "forceDisabled .esl paragraph");
        const int gr = row_with_name(table, "ForcedOff.esp");
        check(table->item(gr, 0)->toolTip().contains(
                  "This game does not currently permit custom plugin loading. "
                  "There may be manual workarounds."),
              "forceDisabled generic paragraph");
      }

      // Game-owned plugins show MO2's raw base data origin name ("Data").
      check(table->item(0, 0)->toolTip().contains("<b>Origin</b>: Data"),
            "game-owned origin renders as Data");

      // Free text truncates at 1024 chars + "..." (MO2 TruncateString).
      {
        const QString vt =
            table->item(row_with_name(table, "Verbose.esp"), 0)->toolTip();
        check(vt.contains(QString::fromStdString(std::string(1024, 'x')) + "..."),
              "author truncated at 1024 chars");
        check(!vt.contains(QString::fromStdString(std::string(1025, 'x'))),
              "author not emitted past the limit");
        check(vt.contains(QString::fromStdString(std::string(1024, 'y')) + "..."),
              "description truncated at 1024 chars");
      }

      // LOOT bullets: exact MO2 order/content/markup, wrapped once.
      {
        const QString lt =
            table->item(row_with_name(table, "Looted.esp"), 0)->toolTip();
        check(lt.contains("<li>Incompatible with Other Mod</li>"),
              "LOOT incompatibility uses the display name");
        check(lt.contains("<li>Incompatible with Nameless.esp</li>"),
              "LOOT incompatibility falls back to the name");
        check(lt.indexOf("Incompatible with") < lt.indexOf("Depends on missing"),
              "incompatibilities sort before missing masters");
        check(lt.contains("<li>Depends on missing GoneMaster.esm</li>"),
              "LOOT missing master bullet");
        check(
            lt.contains("<li>Warning: A <a href=\"http://example.com\">note</a></li>"),
            "LOOT warning bullet keeps raw HTML");
        check(lt.contains("<li>Error: Bad thing</li>"), "LOOT error bullet");
        check(lt.contains("<li>Plain</li>"), "LOOT info bullet has no prefix");
        check(lt.contains("<li>SSEEdit found 3 ITM record(s), 1 deleted "
                          "reference(s) and 2 deleted navmesh(es). extra</li>"),
              "LOOT dirty bullet wording");
        check(lt.contains("<li>? found 0 ITM record(s), 0 deleted reference(s) "
                          "and 0 deleted navmesh(es).</li>"),
              "LOOT dirty bullet with empty utility and info");
        check(lt.contains("<li>Verified clean by SSEEdit</li>"), "LOOT clean bullet");
        check(lt.contains("<ul style=\"margin-top:0px; padding-top:0px; "
                          "margin-left:15px; -qt-list-indent: 0;\">"),
              "LOOT bullets wrapped in the exact MO2 list markup");
        // Emblems: LOOT incompatibilities/missing masters raise the warning,
        // LOOT messages raise information, dirty findings raise edit-clear.
        const QStringList loot_tips =
            table->item(row_with_name(table, "Looted.esp"), 1)
                ->data(ui::PluginsTab::kPluginFlagTooltipsRole)
                .value<QStringList>();
        check(loot_tips.size() == 3, "warning + information + dirty emblems");
        check(loot_tips[0].contains("Incompatible with"),
              "warning emblem covers LOOT findings");
        check(loot_tips[1].contains("Warning:"),
              "information emblem covers LOOT messages");
        check(loot_tips[2].contains("ITM record(s)"),
              "dirty emblem covers cleaning findings");
      }
    }
  }

  // --- Lock context menu (MO2 PluginListContextMenu parity) ---
  {
    tab.set_plugins(plugins);

    // Unlocked user row: offers "Lock load order" and emits lock_requested.
    QMenu menu;
    tab.add_context_menu_actions(menu, row_with_name(table, "SkyUI_SE.esp"));
    auto* lock_act = action_with_text(menu, "Lock load order");
    check(lock_act != nullptr, "unlocked row offers Lock load order");
    check(action_with_text(menu, "Unlock load order") == nullptr,
          "unlocked row offers no Unlock");
    std::vector<std::pair<std::string, bool>> locks;
    QObject::connect(&tab, &ui::PluginsTab::lock_requested,
                     [&](const std::string& name, bool locked) {
                       locks.emplace_back(name, locked);
                     });
    lock_act->trigger();
    check(locks.size() == 1 && locks[0].first == "SkyUI_SE.esp" && locks[0].second,
          "Lock action emits lock_requested(name, true)");

    // Locked row: offers "Unlock load order".
    engine::GamePlugin locked2 = skyui;
    locked2.name               = "Locked2.esp";
    locked2.locked             = true;
    tab.set_plugins({native, locked2});
    QMenu menu2;
    tab.add_context_menu_actions(menu2, row_with_name(table, "Locked2.esp"));
    auto* unlock_act = action_with_text(menu2, "Unlock load order");
    check(unlock_act != nullptr, "locked row offers Unlock load order");
    check(action_with_text(menu2, "Lock load order") == nullptr,
          "locked row offers no Lock");
    locks.clear();
    unlock_act->trigger();
    check(locks.size() == 1 && locks[0].first == "Locked2.esp" && !locks[0].second,
          "Unlock action emits lock_requested(name, false)");

    // Core row (force_loaded): no lock actions at all.
    QMenu menu3;
    tab.add_context_menu_actions(menu3, row_with_name(table, "Skyrim.esm"));
    check(menu3.actions().isEmpty(), "core row has no lock actions");
  }

  // --- MO2-style plugin counter (PluginListView::updatePluginCount parity) ---
  // The header row above the table carries a Refresh button (left) and a
  // single counter (right) that shows enabled + filter-visible plugins, with
  // an active/total breakdown by type in the tooltip. Classification order
  // matches MO2: medium > light (ext or flag) > master (ext or flag) >
  // regular.
  {
    engine::GamePlugin master = native;  // Skyrim.esm: master, enabled
    engine::GamePlugin lite;             // .esl ext: light, enabled
    lite.name          = "Lite.esl";
    lite.has_light_ext = true;
    lite.enabled       = true;
    engine::GamePlugin lite_flag;  // ESL-flagged .esp: light, disabled
    lite_flag.name             = "LiteFlag.esp";
    lite_flag.is_light_flagged = true;
    lite_flag.enabled          = false;
    engine::GamePlugin reg_on;  // regular, enabled
    reg_on.name    = "RegularOn.esp";
    reg_on.enabled = true;
    engine::GamePlugin reg_off;  // regular, disabled
    reg_off.name    = "RegularOff.esp";
    reg_off.enabled = false;
    engine::GamePlugin medium;  // medium (ESH), enabled
    medium.name              = "Medium.esm";
    medium.is_medium_flagged = true;
    medium.enabled           = true;

    const std::vector<engine::GamePlugin> count_set = {master, lite,    lite_flag,
                                                       reg_on, reg_off, medium};

    auto* counter     = tab.findChild<QLCDNumber*>("mo2CounterLabel");
    auto* refresh_btn = tab.findChild<QPushButton*>("pluginRefreshBtn");
    check(counter != nullptr, "MO2 counter label present above the table");
    check(refresh_btn != nullptr, "Refresh button present above the table");

    tab.set_plugins(count_set);
    // 6 rows, 4 enabled (master, lite, reg_on, medium), all visible.
    check(counter->intValue() == 4, "counter shows enabled+visible count");
    check(counter->digitCount() == 4, "counter zero-pads to 4 digits ([ 0000 ])");

    // Tooltip breakdown mirrors MO2: All 4/6, ESMs 1/1, ESPs 1/2,
    // ESMs+ESPs 2/3, ESLs 1/2, and the ESH row (a medium plugin exists).
    const QString tip = counter->toolTip();
    check(tip.contains("<tr><td>All plugins:</td><td align=\"right\">4</td>"
                       "<td align=\"right\">6</td></tr>") &&
              tip.contains("<tr><td>ESMs:</td><td align=\"right\">1</td>"
                           "<td align=\"right\">1</td></tr>") &&
              tip.contains("<tr><td>ESPs:</td><td align=\"right\">1</td>"
                           "<td align=\"right\">2</td></tr>") &&
              tip.contains("<tr><td>ESMs+ESPs:</td><td align=\"right\">2</td>"
                           "<td align=\"right\">3</td></tr>"),
          "counter tooltip All/ESMs/ESPs/ESMs+ESPs active+total rows");
    check(tip.contains("<tr><td>ESLs:</td><td align=\"right\">1</td>"
                       "<td align=\"right\">2</td></tr>"),
          "counter tooltip ESL row");
    check(tip.contains("<tr><td>ESHs:</td><td align=\"right\">1</td>"
                       "<td align=\"right\">1</td></tr>"),
          "counter tooltip ESH row when a medium plugin exists");

    // Filter parity: a row hidden by the text filter is excluded.
    const int reg_row = row_with_name(table, "RegularOn.esp");
    table->setRowHidden(reg_row, true);
    tab.refresh_counters();
    check(counter->intValue() == 3, "counter excludes filter-hidden rows");
    table->setRowHidden(reg_row, false);
    tab.refresh_counters();
    check(counter->intValue() == 4, "counter returns after unhide");

    // Enable toggles (sync_enabled path, no rebuild) update the counter.
    std::vector<engine::GamePlugin> count_set2 = count_set;
    count_set2[2].enabled                      = true;  // LiteFlag.esp -> 5 active
    tab.sync_enabled(count_set2);
    check(counter->intValue() == 5, "counter follows sync_enabled toggles");
    tab.sync_enabled(count_set);  // revert
    check(counter->intValue() == 4, "counter reverts with sync_enabled");

    // The Refresh button asks for a plugins-only rescan.
    bool refreshed = false;
    QObject::connect(&tab, &ui::PluginsTab::refresh_requested, [&refreshed]() {
      refreshed = true;
    });
    tab.resize(640, 400);
    tab.show();
    QApplication::processEvents();
    QTest::mouseClick(refresh_btn, Qt::LeftButton);
    QApplication::processEvents();
    check(refreshed, "Refresh button emits refresh_requested");
    tab.hide();
  }
}
