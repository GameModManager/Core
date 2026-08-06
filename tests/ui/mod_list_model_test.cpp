// Offscreen GUI test for the ModListModel game-native (unmanaged) band.
//
// Regression for the "Unmanaged mod landed below user mods" bug: game-native
// entries must be a pinned top band that user mods can never move above. The
// band is enforced by three layers, all covered here:
//   - move_mod(): a game-native source is a no-op, and a user-mod target is
//     clamped to native_band_bottom() (never into the band),
//   - mimeData(): game-native rows are not drag sources,
//   - dropMimeData(): a drop aimed above the band is clamped down to it, and
//     a native-only source is rejected outright.
//
// The load-time ordering (priority assignment + band-aware sort) lives in
// MainWindow::load_mods_from_game()/load_order() and is verified manually;
// the model-level guards below make the band unbreakable at interaction time.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, no network.
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/column_toggle_header.h"
#include "ui/settings/settings.h"
#include "engine/theme/icon_manager.h"

#include <QApplication>
#include <QBrush>
#include <QContextMenuEvent>
#include <QFont>
#include <QIcon>
#include <QMenu>
#include <QMimeData>
#include <QModelIndexList>
#include <QTableView>
#include <QTimer>

#include <cstdio>
#include <filesystem>
#include <string>

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

static int row_with_id(const ui::ModListModel& model, const char* id) {
    const auto& mods = model.mods();
    for (int i = 0; i < mods.size(); ++i)
        if (mods[i].id == QLatin1String(id)) return i;
    return -1;
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Deterministic "yyyy-MM-dd HH:mm:ss" rendering: format_epoch_ts uses local
    // time, so pin the test process to UTC.
    qputenv("TZ", "UTC");
    tzset();
    const std::filesystem::path cfg = "/tmp/gmm_mod_list_model/config";
    std::filesystem::remove_all("/tmp/gmm_mod_list_model");
    std::filesystem::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

    ui::ModListModel model;

    // Simulate the post-load state: game-native band on top, user mods below.
    QVector<ui::ModEntry> entries;
    for (const char* id : {"Skyrim.esm", "Update.esm"}) {
        ui::ModEntry n;
        n.id = QString::fromLatin1(id);
        n.name = n.id;
        n.enabled = true;
        n.is_game_native = true;
        entries.append(n);
    }
    for (const char* id : {"SkyUI", "Enemy NPCs"}) {
        ui::ModEntry u;
        u.id = QString::fromLatin1(id);
        u.name = u.id;
        u.enabled = true;
        entries.append(u);
    }
    ui::ModEntry ow;
    ow.id = ui::kOverwriteModId;
    ow.name = ui::kOverwriteModName;
    ow.enabled = true;
    ow.is_overwrite = true;
    entries.append(ow);
    model.reset_with_order(entries);

    check(model.native_band_bottom() == 2, "native band occupies rows 0-1");
    check(row_with_id(model, "Skyrim.esm") == 0 &&
              row_with_id(model, "Update.esm") == 1 &&
              row_with_id(model, "SkyUI") == 2 &&
              row_with_id(model, "Enemy NPCs") == 3,
          "band-first layout");

    // Separator display: the Name cell is the fold arrow + the separator name
    // (regression: the MO2-look pass dropped the name, leaving only the arrow);
    // EditRole still carries the raw name for name-based lookups.
    {
        ui::ModEntry sep;
        sep.id = QStringLiteral("Testing_separator");
        sep.name = QStringLiteral("Testing");
        sep.enabled = true;
        sep.is_separator = true;
        sep.separator_color = "#888888";
        model.add_separator(sep.id, sep.name, sep.separator_color);
        int r = row_with_id(model, "Testing_separator");
        check(r >= 0, "separator row present");
        if (r >= 0) {
            QVariant disp = model.data(model.index(r, ui::ModListModel::Name), Qt::DisplayRole);
            check(disp.isValid() && disp.toString() == QStringLiteral("\u25BC Testing"),
                  "separator DisplayRole shows arrow + name");
        QVariant edit = model.data(model.index(r, ui::ModListModel::Name), Qt::EditRole);
        check(edit.isValid() && edit.toString() == QStringLiteral("Testing"),
              "separator EditRole carries raw name");
    }

    // Fold-persistence regression: toggling a separator must announce
    // mod_list_changed so MainWindow persists instance.toml's
    // folded_separators. Without the emit, a fold/unfold never reached the
    // disk and every reopen/relaunch reset the separator states (user report).
    {
        const int sep_row = row_with_id(model, "Testing_separator");
        check(sep_row >= 0, "separator row present for fold test");
        int change_count = 0;
        QObject::connect(&model, &ui::ModListModel::mod_list_changed,
                         [&]() { ++change_count; });
        check(!model.mods()[sep_row].folded, "separator starts unfolded");
        model.set_folded(sep_row, true);
        check(model.mods()[sep_row].folded, "set_folded(true) folds the separator");
        check(change_count == 1, "fold emits mod_list_changed (persist trigger)");
        model.set_folded(sep_row, true);
        check(change_count == 1, "no-op fold does not re-emit");
        model.set_folded(sep_row, false);
        check(!model.mods()[sep_row].folded, "set_folded(false) unfolds");
        check(change_count == 2, "unfold emits mod_list_changed");
        model.set_folded(0, true);
        check(!model.mods()[0].folded && change_count == 2,
              "set_folded on a non-separator row is a no-op");
        // The lambda captures change_count by reference; tear the connection
        // down before this block ends, or a later mod_list_changed emission
        // (rename_mod_in_place, moves, ...) writes through the dangling ref.
        QObject::disconnect(&model, &ui::ModListModel::mod_list_changed,
                            nullptr, nullptr);
    }
    }

    // move_mod(): a game-native source never moves.
    model.move_mod("Skyrim.esm", 4);
    check(row_with_id(model, "Skyrim.esm") == 0 && model.native_band_bottom() == 2,
          "native mod move is a no-op");

    // move_mod(): a user mod aimed at the top clamps to just below the band.
    model.move_mod("Enemy NPCs", 0);
    check(row_with_id(model, "Enemy NPCs") == 2 && model.native_band_bottom() == 2,
          "user mod move clamps to band bottom");
    check(row_with_id(model, "Skyrim.esm") == 0 &&
              row_with_id(model, "Update.esm") == 1,
          "band order preserved after clamped move");

    // mimeData(): game-native rows are not drag sources.
    // Current order: Skyrim.esm(0), Update.esm(1), Enemy NPCs(2), SkyUI(3)
    QModelIndexList idxs;
    for (int r = 0; r < 4; ++r)
        idxs.append(model.index(r, ui::ModListModel::Name));
    QMimeData* mime = model.mimeData(idxs);
    check(mime != nullptr, "mime data produced");
    if (mime) {
        const QByteArray enc = mime->data(QLatin1String(ui::kModListMimeType));
        check(!enc.split(',').contains(QByteArrayLiteral("0")) &&
                  !enc.split(',').contains(QByteArrayLiteral("1")),
              "native rows excluded from drag");
        check(enc.split(',').contains(QByteArrayLiteral("2")) &&
                  enc.split(',').contains(QByteArrayLiteral("3")),
              "user rows included in drag");
    }
    delete mime;

    // dropMimeData(): a drop aimed above the band clamps to band bottom.
    QMimeData drop;
    drop.setData(QLatin1String(ui::kModListMimeType), QByteArrayLiteral("3"));  // SkyUI
    check(model.dropMimeData(&drop, Qt::MoveAction, 0, 0, {}),
          "user-mod drop accepted");
    check(row_with_id(model, "Skyrim.esm") == 0 &&
              row_with_id(model, "Update.esm") == 1 &&
              row_with_id(model, "SkyUI") == 2 &&
              row_with_id(model, "Enemy NPCs") == 3,
          "drop clamped below the native band");

    // dropMimeData(): a native-only source is rejected.
    QMimeData bad;
    bad.setData(QLatin1String(ui::kModListMimeType), QByteArrayLiteral("0"));  // Skyrim.esm
    check(!model.dropMimeData(&bad, Qt::MoveAction, 3, 0, {}),
          "native-only drop rejected");
    check(row_with_id(model, "Skyrim.esm") == 0,
          "band intact after rejected drop");

    // --- Plugin-selected mod highlight (MO2 "mod contains selected file") ---
    {
        const QColor hl = Settings::instance().modlist_contains_file();
        model.set_highlighted_mods({QStringLiteral("SkyUI")});

        // Scroll mark role: the highlighted mod returns the color, others none.
        const int hl_row = row_with_id(model, "SkyUI");
        const int other_row = row_with_id(model, "Enemy NPCs");
        const QVariant mark = model.data(
            model.index(hl_row, ui::ModListModel::Name), ui::ModListModel::kScrollMarkRole);
        check(mark.canConvert<QColor>() && mark.value<QColor>() == hl,
              "scroll mark for highlighted mod");
        const QVariant no_mark = model.data(
            model.index(other_row, ui::ModListModel::Name), ui::ModListModel::kScrollMarkRole);
        check(!no_mark.isValid() || !no_mark.value<QColor>().isValid(),
              "no scroll mark for unhighlighted mod");

        // BackgroundRole: the highlighted row tints.
        const QVariant bg = model.data(
            model.index(hl_row, ui::ModListModel::Flags), Qt::BackgroundRole);
        check(bg.canConvert<QBrush>() && bg.value<QBrush>().color() == hl,
              "background tint for highlighted mod");

        // Highlight beats the conflict highlight (MO2 markerColor precedence).
        ui::ConflictPairs pairs;
        pairs.loses_to << QStringLiteral("SkyUI");
        model.set_conflict_pairs({{QStringLiteral("Enemy NPCs"), pairs}});
        model.set_selected_mod(QStringLiteral("Enemy NPCs"));
        const QVariant conflict_bg = model.data(
            model.index(hl_row, ui::ModListModel::Flags), Qt::BackgroundRole);
        check(conflict_bg.canConvert<QBrush>() &&
                  conflict_bg.value<QBrush>().color() == hl,
              "plugin highlight beats conflict color");

        // Clearing the highlight reveals the conflict color underneath.
        model.set_highlighted_mods({});
        const QVariant after_clear = model.data(
            model.index(hl_row, ui::ModListModel::Flags), Qt::BackgroundRole);
        check(after_clear.canConvert<QBrush>() &&
                  after_clear.value<QBrush>().color() ==
                      Settings::instance().modlist_overwriting_loose(),
              "clearing reveals conflict color");
        model.set_selected_mod({});
        model.set_conflict_pairs({});
    }

    // Separator mark gate: with separator coloring off a separator row yields
    // no mark, while a highlighted mod's mark is independent of it.
    {
        Settings::instance().set_color_separator_scrollbar(false);
        const QVariant sep_mark = model.data(
            model.index(row_with_id(model, "Testing_separator"), ui::ModListModel::Name),
            ui::ModListModel::kScrollMarkRole);
        check(!sep_mark.isValid() || !sep_mark.value<QColor>().isValid(),
              "separator mark hidden when separator coloring off");
        model.set_highlighted_mods({QStringLiteral("SkyUI")});
        const QVariant hl_mark = model.data(
            model.index(row_with_id(model, "SkyUI"), ui::ModListModel::Name),
            ui::ModListModel::kScrollMarkRole);
        check(hl_mark.canConvert<QColor>() &&
                  hl_mark.value<QColor>() == Settings::instance().modlist_contains_file(),
              "highlight mark independent of separator coloring");
        Settings::instance().set_color_separator_scrollbar(true);
        model.set_highlighted_mods({});
    }

    // --- MO2-parity inline rename (EditRole) + separator colors ---
    {
        const int sep_row = row_with_id(model, "Testing_separator");
        const int mod_row = row_with_id(model, "SkyUI");
        const int ow_row = row_with_id(model, ui::kOverwriteModId);
        check(sep_row >= 0 && mod_row >= 0 && ow_row >= 0,
              "rows located for rename tests");

        // ItemIsEditable on the Name column for separators and regular mods,
        // never for Overwrite / game-native rows.
        const auto sep_flags = model.flags(model.index(sep_row, ui::ModListModel::Name));
        check(sep_flags & Qt::ItemIsEditable, "separator Name cell is editable");
        const auto mod_flags = model.flags(model.index(mod_row, ui::ModListModel::Name));
        check(mod_flags & Qt::ItemIsEditable, "mod Name cell is editable");
        const auto ow_flags = model.flags(model.index(ow_row, ui::ModListModel::Name));
        check(!(ow_flags & Qt::ItemIsEditable), "Overwrite Name cell is not editable");
        const int native_row = row_with_id(model, "Skyrim.esm");
        const auto nat_flags = model.flags(model.index(native_row, ui::ModListModel::Name));
        check(!(nat_flags & Qt::ItemIsEditable), "game-native Name cell is not editable");

        // setData(EditRole) emits rename_requested with the trimmed name and
        // does not mutate the row itself (the window handler owns the rename).
        QString requested;
        int requested_row = -1;
        QObject::connect(&model, &ui::ModListModel::rename_requested,
                         [&](int row, const QString& name) {
                             requested_row = row;
                             requested = name;
                         });
        check(model.setData(model.index(mod_row, ui::ModListModel::Name),
                            QStringLiteral("  SkyUI  "), Qt::EditRole),
              "setData EditRole accepted");
        check(requested_row == mod_row && requested == QStringLiteral("SkyUI"),
              "rename_requested carries row + trimmed name");
        check(model.mods()[mod_row].name == QStringLiteral("SkyUI"),
              "setData EditRole does not mutate the row");

        // Overwrite / game-native rows reject edits outright.
        check(!model.setData(model.index(ow_row, ui::ModListModel::Name),
                             QStringLiteral("x"), Qt::EditRole),
              "Overwrite rejects EditRole");
        check(!model.setData(model.index(native_row, ui::ModListModel::Name),
                             QStringLiteral("x"), Qt::EditRole),
              "game-native row rejects EditRole");

        // Separators are still not checkable/toggleable.
        check(!model.setData(model.index(sep_row, ui::ModListModel::Name),
                             Qt::Checked, Qt::CheckStateRole),
              "separator rejects CheckStateRole");
        QObject::disconnect(&model, &ui::ModListModel::rename_requested, nullptr, nullptr);
    }

    // rename_mod_in_place: keeps the row where it is (id + name updated).
    {
        const int before = row_with_id(model, "SkyUI");
        model.rename_mod_in_place(before, QStringLiteral("SkyUI2"), QStringLiteral("SkyUI2"));
        check(row_with_id(model, "SkyUI") < 0 && row_with_id(model, "SkyUI2") == before,
              "rename in place preserves the row position");
        check(model.mods()[before].name == QStringLiteral("SkyUI2"),
              "rename in place updates the display name");
    }

    // set_mod_color / clear_mod_color drive the separator BackgroundRole.
    {
        const int sep_row = row_with_id(model, "Testing_separator");
        const QColor teal(0, 128, 128, 200);
        model.set_mod_color(QStringLiteral("Testing_separator"), teal);
        const QVariant bg = model.data(
            model.index(sep_row, ui::ModListModel::Name), Qt::BackgroundRole);
        check(bg.canConvert<QBrush>() && bg.value<QBrush>().color() == teal,
              "set_mod_color tints the separator row");
        check(model.mods()[sep_row].separator_color == teal.name(QColor::HexArgb),
              "set_mod_color stores HexArgb in the model entry");

        model.clear_mod_color(QStringLiteral("Testing_separator"));
        check(model.mods()[sep_row].separator_color.isEmpty(),
              "clear_mod_color empties the stored color");
        const QVariant bg2 = model.data(
            model.index(sep_row, ui::ModListModel::Name), Qt::BackgroundRole);
        check(bg2.canConvert<QBrush>() &&
                  bg2.value<QBrush>().color() == QColor(QStringLiteral("#888888")),
              "cleared separator falls back to the render default");
    }

    // Settings: previous_separator_color round-trip (MO2 previousSeparatorColor).
    {
        Settings::instance().remove_previous_separator_color();
        check(!Settings::instance().previous_separator_color().has_value(),
              "previous_separator_color empty by default");
        const QColor lavender(200, 160, 220, 128);
        Settings::instance().set_previous_separator_color(lavender);
        auto stored = Settings::instance().previous_separator_color();
        check(stored.has_value() && *stored == lavender,
              "previous_separator_color round-trips through QSettings");
        Settings::instance().remove_previous_separator_color();
        check(!Settings::instance().previous_separator_color().has_value(),
              "previous_separator_color removed");
    }

    // --- Invalid-data / no-metadata flags (MO2 FLAG_INVALID parity) ---
    {
        model.add_mod(QStringLiteral("GhostMod"), QStringLiteral("GhostMod"), QString(), -1);
        const int gr = row_with_id(model, "GhostMod");
        check(gr >= 0, "flag-test mod row present");

        // invalid-data only: flag icon + tooltip + italic gray name (MO2).
        model.set_invalid_data(QStringLiteral("GhostMod"), true);
        QList<QIcon> icons = model.data(
            model.index(gr, ui::ModListModel::Flags),
            ui::ModListModel::kFlagIconsRole).value<QList<QIcon>>();
        check(icons.size() == 1, "invalid-data adds one flag icon");
        const QString tip = model.data(
            model.index(gr, ui::ModListModel::Flags), Qt::ToolTipRole).toString();
        check(tip.contains(QStringLiteral("No valid game data")),
              "invalid-data tooltip");
        const QVariant fnt = model.data(
            model.index(gr, ui::ModListModel::Name), Qt::FontRole);
        check(fnt.canConvert<QFont>() && fnt.value<QFont>().italic(),
              "invalid-data name is italic");
        const QVariant fg = model.data(
            model.index(gr, ui::ModListModel::Name), Qt::ForegroundRole);
        check(fg.canConvert<QColor>() && fg.value<QColor>().isValid(),
              "invalid-data name is tinted");

        // no_metadata: same single icon, own tooltip line; name stays normal.
        model.set_no_metadata(QStringLiteral("GhostMod"), true);
        icons = model.data(
            model.index(gr, ui::ModListModel::Flags),
            ui::ModListModel::kFlagIconsRole).value<QList<QIcon>>();
        check(icons.size() == 1, "both flags still render a single icon");
        const QString tip2 = model.data(
            model.index(gr, ui::ModListModel::Flags), Qt::ToolTipRole).toString();
        check(tip2.contains(QStringLiteral("Not installed by the manager")),
              "no_metadata tooltip");
        check(tip2.contains(QStringLiteral("No valid game data")),
              "tooltip lists both reasons when both flags are set");

        // Clearing both removes the icon and restores the normal name.
        model.set_invalid_data(QStringLiteral("GhostMod"), false);
        model.set_no_metadata(QStringLiteral("GhostMod"), false);
        icons = model.data(
            model.index(gr, ui::ModListModel::Flags),
            ui::ModListModel::kFlagIconsRole).value<QList<QIcon>>();
        check(icons.isEmpty(), "clearing the flags removes the icon");
        const QVariant fnt2 = model.data(
            model.index(gr, ui::ModListModel::Name), Qt::FontRole);
        check(!fnt2.isValid() || !fnt2.value<QFont>().italic(),
              "clearing invalid-data restores the non-italic name");
    }

    {
        // Column set (P8.4): exactly the 10 columns in display order.
        check(model.columnCount() == 10, "mod list exposes 10 columns");
        const char* labels[] = {"Name", "Conflicts", "Flags", "Category", "Source",
                                "Source ID", "Version", "Installation", "Changed",
                                "Priority"};
        for (int c = 0; c < 10; ++c) {
            const QVariant hd = model.headerData(c, Qt::Horizontal, Qt::DisplayRole);
            check(hd.isValid() && hd.toString() == QLatin1String(labels[c]),
                  "header label for new column");
        }

        // add_mod carries the filesystem timestamps into the model entry.
        model.add_mod(QStringLiteral("Timed"), QStringLiteral("Timed Mod"),
                      QStringLiteral("1.0"), 7, false, 1700000000, 1700005000);
        const int tr = row_with_id(model, "Timed");
        check(tr >= 0, "timed mod row present");
        check(model.mods()[tr].installation_ts == 1700000000 &&
                  model.mods()[tr].changed_ts == 1700005000,
              "add_mod stores install/changed timestamps");

        // Category renders in the Category column.
        model.set_category(QStringLiteral("Timed"), QStringLiteral("Gameplay"));
        const QVariant cat = model.data(
            model.index(tr, ui::ModListModel::Category), Qt::DisplayRole);
        check(cat.isValid() && cat.toString() == QLatin1String("Gameplay"),
              "set_category renders in the Category column");
        model.set_category(QStringLiteral("Timed"), QStringLiteral("Gameplay"));
        model.set_category(QStringLiteral("Timed"), QStringLiteral("UI"));
        check(model.mods()[tr].category == QStringLiteral("UI"),
              "set_category updates an existing value");

        // set_timestamps updates the entry and the two date columns.
        model.set_timestamps(QStringLiteral("Timed"), 1700000000, 1700005000);
        const QVariant inst = model.data(
            model.index(tr, ui::ModListModel::Installation), Qt::DisplayRole);
        const QVariant chg = model.data(
            model.index(tr, ui::ModListModel::Changed), Qt::DisplayRole);
        check(inst.toString() == QLatin1String("2023-11-14 22:13:20"),
              "Installation renders folder birth time as local datetime");
        check(chg.toString() == QLatin1String("2023-11-14 23:36:40"),
              "Changed renders folder mtime as local datetime");
        model.set_timestamps(QStringLiteral("Timed"), 0, 0);
        const QVariant inst0 = model.data(
            model.index(tr, ui::ModListModel::Installation), Qt::DisplayRole);
        check(inst0.toString().isEmpty(),
              "zero timestamps render empty cells");

        // Source column: ID text + vendor icon + tooltip (MO2 COL_GAME).
        model.set_source_info(QStringLiteral("Timed"), QStringLiteral("nexusmods"),
                              QStringLiteral("12345"), QStringLiteral("https://nxm/"));
        const QVariant sid = model.data(
            model.index(tr, ui::ModListModel::SourceId), Qt::DisplayRole);
        check(sid.isValid() && sid.toString() == QLatin1String("12345"),
              "Source ID renders the numeric id");
        const QVariant tip = model.data(
            model.index(tr, ui::ModListModel::Source), Qt::ToolTipRole);
        check(tip.isValid() && tip.toString() == QLatin1String("nexusmods"),
              "Source tooltip shows the vendor name");
        // Icon only when the icon pack actually resolves the vendor badge:
        // whatever resolve_icon yields, the model's DecorationRole must match
        // (null in a hermetic test with no icon pack, non-null otherwise).
        const QIcon vendor = engine::IconManager::instance().resolve_icon("nexusmods");
        const QVariant dec = model.data(
            model.index(tr, ui::ModListModel::Source), Qt::DecorationRole);
        const QIcon dec_icon = dec.canConvert<QIcon>() ? dec.value<QIcon>() : QIcon();
        check(dec_icon.isNull() == vendor.isNull(),
              "Source DecorationRole matches the resolved vendor icon");

        // Version and Priority keep rendering after the enum reorder.
        const QVariant ver = model.data(
            model.index(tr, ui::ModListModel::Version), Qt::DisplayRole);
        check(ver.isValid() && ver.toString() == QLatin1String("1.0"),
              "Version renders after column reorder");
        const QVariant prio = model.data(
            model.index(tr, ui::ModListModel::Priority), Qt::DisplayRole);
        check(prio.isValid() && prio.toInt() == 7,
              "Priority renders after column reorder");
    }

    {
        // ColumnToggleHeaderView: lock API + the context-menu contract the
        // Name column relies on — locked entries render checked+disabled, an
        // unlocked user toggle hides the section and emits section_toggled.
        QTableView view;
        auto* hdr = new ui::ColumnToggleHeaderView(Qt::Horizontal, &view);
        hdr->set_column_labels({"Name", "Conflicts", "Flags"});
        hdr->set_locked_section(0);
        view.setModel(&model);
        view.setHorizontalHeader(hdr);
        check(hdr->count() == model.columnCount(),
              "header follows the model column count");
        check(hdr->is_locked(0), "lockable section reported locked");
        check(!hdr->is_locked(1), "unlocked section reported unlocked");
        hdr->set_section_tooltips({"tip0", QString(), "tip2"});
        check(hdr->section_tooltip(0) == QLatin1String("tip0") &&
                  hdr->section_tooltip(1).isEmpty() &&
                  hdr->section_tooltip(2) == QLatin1String("tip2"),
              "per-section tooltips map by logical index");

        // Drive the real context menu hermetically: the menu's nested event
        // loop is closed by a timer that inspects + triggers actions while
        // the menu is open. The delay must outlive event dispatch (a 0-ms
        // timer fires before a posted event is processed).
        QVector<QPair<bool, bool>> menu_state;  // (enabled, checked)
        QVector<int> toggled;
        QObject::connect(hdr, &ui::ColumnToggleHeaderView::section_toggled, &view,
                         [&toggled](int logical, bool hidden) {
                             toggled.append(logical);
                             toggled.append(hidden ? 1 : 0);
                         });
        QTimer::singleShot(100, [&]() {
            QMenu* m = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!m) return;
            const auto actions = m->actions();
            for (int i = 0; i < actions.size() && i < 3; ++i)
                menu_state.append({actions.at(i)->isEnabled(),
                                   actions.at(i)->isChecked()});
            if (actions.size() > 1) actions.at(1)->trigger();
            m->close();
        });
        // Safety net: never let the nested menu loop hang the test.
        QTimer::singleShot(2000, []() {
            if (QWidget* w = QApplication::activePopupWidget()) w->close();
        });
        auto* cme = new QContextMenuEvent(QContextMenuEvent::Mouse, QPoint(5, 5),
                                          QPoint(5, 5));
        QCoreApplication::postEvent(hdr->viewport(), cme);
        QApplication::processEvents();
        check(menu_state.size() == 3, "context menu offers one action per column");
        check(menu_state.size() == 3 && !menu_state.at(0).first &&
                  menu_state.at(0).second,
              "locked column renders checked + disabled");
        check(toggled.size() == 2 && toggled.at(0) == 1 && toggled.at(1) == 1,
              "unlocked toggle emits section_toggled(hidden)");
        check(hdr->isSectionHidden(1),
              "user toggle hides the unlocked section");
        check(!hdr->isSectionHidden(0),
              "locked section stays visible");

        // The lock is menu-enforced by design: programmatic hide still works
        // (the restore path uses setSectionHidden directly).
        hdr->setSectionHidden(0, true);
        check(hdr->isSectionHidden(0),
              "programmatic setSectionHidden bypasses the menu lock");
        QObject::disconnect(hdr, nullptr, &view, nullptr);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}