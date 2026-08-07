// Offscreen GUI test for the ModListModel game-native (unmanaged) band.
//
// Regression for the "Unmanaged mod landed below user mods" bug: game-native
// entries must be a pinned top band that user mods can never move above. The
// band is enforced by three layers, all covered here:
//   - move_mod(): a game-native source is a no-op, and a user-mod target is
//     clamped to just below the band (never into the band); a separator may
//     move above the band (and an in-band separator target snaps to it),
//   - mimeData(): game-native rows are not drag sources,
//   - dropMimeData(): a drop aimed above the band is clamped down to it (for
//     non-separator drags) while a separator-only drag may land above it, and
//     a native-only source is rejected outright.
//
// The load-time ordering (priority assignment + band-aware sort) lives in
// MainWindow::load_mods_from_game()/load_order() and is verified manually;
// the model-level guards below make the band unbreakable at interaction time.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, no network.
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"
#include "ui/widgets/column_toggle_header.h"
#include "ui/settings/settings.h"
#include "engine/theme/icon_manager.h"

#include <QApplication>
#include <QBrush>
#include <QContextMenuEvent>
#include <QFont>
#include <QIcon>
#include <QImage>
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

    check(model.native_band_first() == 0 && model.native_band_last() == 1,
          "native band occupies rows 0-1");
    check(row_with_id(model, "Skyrim.esm") == 0 &&
              row_with_id(model, "Update.esm") == 1 &&
              row_with_id(model, "SkyUI") == 2 &&
              row_with_id(model, "Enemy NPCs") == 3,
          "band-first layout");

    // Separator display: the fold arrow lives in its own Fold column (left of
    // Name); the Name cell is the plain separator name (regression: the arrow
    // used to be a "▼ "/"▶ " prefix on Name). EditRole still carries the raw
    // name for name-based lookups.
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
            QVariant name = model.data(model.index(r, ui::ModListModel::Name), Qt::DisplayRole);
            check(name.isValid() && name.toString() == QStringLiteral("Testing"),
                  "separator Name cell is the plain name (no arrow prefix)");
            QVariant edit = model.data(model.index(r, ui::ModListModel::Name), Qt::EditRole);
            check(edit.isValid() && edit.toString() == QStringLiteral("Testing"),
                  "separator EditRole carries raw name");
        }
        // This separator was appended between Enemy NPCs and Overwrite -> empty
        // band -> no content to hide -> Fold cell empty and not foldable.
        check(!model.has_content(r), "appended separator has an empty band");
        if (r >= 0) {
            QVariant fold = model.data(model.index(r, ui::ModListModel::Fold), Qt::DisplayRole);
            check(fold.isValid() && fold.toString().isEmpty(),
                  "empty-band separator shows no arrow in Fold column");
        }
        // Non-separator rows never carry an arrow.
        const int skyui = row_with_id(model, "SkyUI");
        if (skyui >= 0) {
            QVariant fold = model.data(model.index(skyui, ui::ModListModel::Fold), Qt::DisplayRole);
            check(fold.toString().isEmpty(),
                  "regular mod Fold cell is empty");
            check(!model.has_content(skyui),
                  "regular mod reports no fold content");
        }
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

    // Fold arrow gating (band rule): a separator sitting above mods shows the
    // glyph in the Fold column and it flips with fold state; a separator whose
    // band ends at Overwrite has nothing to hide.
    {
        ui::ModListModel m2;
        QVector<ui::ModEntry> e2;
        ui::ModEntry s;
        s.id = QStringLiteral("Section");
        s.name = QStringLiteral("Section");
        s.enabled = true;
        s.is_separator = true;
        s.separator_color = "#888888";
        e2.append(s);
        ui::ModEntry mod;
        mod.id = QStringLiteral("ModA");
        mod.name = QStringLiteral("ModA");
        mod.enabled = true;
        e2.append(mod);
        ui::ModEntry ow2;
        ow2.id = ui::kOverwriteModId;
        ow2.name = ui::kOverwriteModName;
        ow2.enabled = true;
        ow2.is_overwrite = true;
        e2.append(ow2);
        m2.reset_with_order(e2);

        check(m2.has_content(0), "separator with a mod below has content");
        const QVariant fold_open =
            m2.data(m2.index(0, ui::ModListModel::Fold), Qt::DisplayRole);
        check(fold_open.isValid() && fold_open.toString() == QStringLiteral("\u25BC"),
              "unfolded separator shows down-arrow in Fold column");
        m2.set_folded(0, true);
        const QVariant fold_closed =
            m2.data(m2.index(0, ui::ModListModel::Fold), Qt::DisplayRole);
        check(fold_closed.isValid() && fold_closed.toString() == QStringLiteral("\u25B6"),
              "folded separator shows right-arrow in Fold column");
        // The Name cell never shows the arrow, regardless of fold state.
        const QVariant name_disp =
            m2.data(m2.index(0, ui::ModListModel::Name), Qt::DisplayRole);
        check(name_disp.isValid() && name_disp.toString() == QStringLiteral("Section"),
              "folded separator Name cell stays the plain name");
        // Fold column alignment is always centered (separator and mod rows).
        const QVariant align_sep = m2.data(
            m2.index(0, ui::ModListModel::Fold), Qt::TextAlignmentRole);
        check(align_sep.isValid() &&
                  align_sep.toInt() == static_cast<int>(Qt::AlignCenter),
              "separator Fold cell centered");
        const QVariant align_mod = m2.data(
            m2.index(1, ui::ModListModel::Fold), Qt::TextAlignmentRole);
        check(align_mod.isValid() &&
                  align_mod.toInt() == static_cast<int>(Qt::AlignCenter),
              "mod Fold cell centered");

        // Overwrite is never content: a separator directly above it hides nothing.
        ui::ModListModel m3;
        QVector<ui::ModEntry> e3;
        ui::ModEntry s3;
        s3.id = QStringLiteral("Solo");
        s3.name = QStringLiteral("Solo");
        s3.enabled = true;
        s3.is_separator = true;
        e3.append(s3);
        ui::ModEntry ow3;
        ow3.id = ui::kOverwriteModId;
        ow3.name = ui::kOverwriteModName;
        ow3.enabled = true;
        ow3.is_overwrite = true;
        e3.append(ow3);
        m3.reset_with_order(e3);
        check(!m3.has_content(0),
              "separator directly above Overwrite has no content");
    }

    // move_mod(): a game-native source never moves.
    model.move_mod("Skyrim.esm", 4);
    check(row_with_id(model, "Skyrim.esm") == 0 && model.native_band_last() == 1,
          "native mod move is a no-op");

    // move_mod(): a user mod aimed at the top clamps to just below the band.
    model.move_mod("Enemy NPCs", 0);
    check(row_with_id(model, "Enemy NPCs") == 2 && model.native_band_last() == 1,
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

    // --- Separators above the game-native band (fold hides the native mods) ---
    {
        ui::ModListModel m2;
        QVector<ui::ModEntry> e2;
        for (const char* id : {"Skyrim.esm", "Update.esm"}) {
            ui::ModEntry n;
            n.id = QString::fromLatin1(id);
            n.name = n.id;
            n.enabled = true;
            n.is_game_native = true;
            e2.append(n);
        }
        ui::ModEntry sep2;
        sep2.id = QStringLiteral("Testing_separator");
        sep2.name = QStringLiteral("Testing");
        sep2.enabled = true;
        sep2.is_separator = true;
        e2.append(sep2);
        ui::ModEntry mod2;
        mod2.id = QStringLiteral("SkyUI");
        mod2.name = QStringLiteral("SkyUI");
        mod2.enabled = true;
        e2.append(mod2);
        ui::ModEntry ow2;
        ow2.id = ui::kOverwriteModId;
        ow2.name = ui::kOverwriteModName;
        ow2.enabled = true;
        ow2.is_overwrite = true;
        e2.append(ow2);
        m2.reset_with_order(e2);
        // Initial: natives(0,1), sep(2), SkyUI(3), overwrite(4).

        // move_mod(): a separator aimed INTO the band snaps to just above it.
        m2.move_mod("Testing_separator", 1);  // between the natives
        check(row_with_id(m2, "Testing_separator") == 0 &&
                  row_with_id(m2, "Skyrim.esm") == 1 &&
                  row_with_id(m2, "Update.esm") == 2,
              "in-band separator move snaps above the band");
        check(m2.native_band_first() == 1 && m2.native_band_last() == 2,
              "band stays contiguous after snap");

        // move_mod(): a separator moved below the band stays below it.
        m2.move_mod("Testing_separator", 3);  // above SkyUI, below the band
        check(row_with_id(m2, "Testing_separator") == 3 &&
                  row_with_id(m2, "Skyrim.esm") == 0,
              "separator below the band stays below");
        check(m2.native_band_first() == 0 && m2.native_band_last() == 1,
              "band back on top after separator moves down");

        // dropMimeData(): a separator-only drop may land above the band.
        QMimeData dsep;
        dsep.setData(QLatin1String(ui::kModListMimeType),
                     QByteArrayLiteral("3"));  // Testing_separator
        check(m2.dropMimeData(&dsep, Qt::MoveAction, 0, 0, {}),
              "separator-only drop above the band accepted");
        check(row_with_id(m2, "Testing_separator") == 0 &&
                  row_with_id(m2, "Skyrim.esm") == 1,
              "separator drop lands above the band");

        // dropMimeData(): a separator-only drop INTO the band snaps to the top.
        QMimeData dsep2;
        dsep2.setData(QLatin1String(ui::kModListMimeType),
                      QByteArrayLiteral("0"));  // Testing_separator
        check(m2.dropMimeData(&dsep2, Qt::MoveAction, 1, 0, {}),
              "separator-only in-band drop accepted");
        check(row_with_id(m2, "Testing_separator") == 0 &&
                  row_with_id(m2, "Skyrim.esm") == 1,
              "in-band separator drop snaps above the band");

        // dropMimeData(): a mixed (separator + mod) drag stays below the band.
        QMimeData dmixed;
        dmixed.setData(QLatin1String(ui::kModListMimeType),
                       QByteArrayLiteral("0,3"));  // sep + SkyUI
        check(m2.dropMimeData(&dmixed, Qt::MoveAction, 0, 0, {}),
              "mixed drag accepted");
        check(row_with_id(m2, "Testing_separator") != 0,
              "mixed drag never lands above the band");
        check(m2.native_band_first() == 0 && m2.native_band_last() == 1,
              "band back on top after mixed drag");
    }

    // --- Center text on separators (Theme > Design, default on) ---
    {
        ui::ModListModel m3;
        QVector<ui::ModEntry> e3;
        for (const char* id : {"Skyrim.esm"}) {
            ui::ModEntry n;
            n.id = QString::fromLatin1(id);
            n.name = n.id;
            n.enabled = true;
            n.is_game_native = true;
            e3.append(n);
        }
        ui::ModEntry sep3;
        sep3.id = QStringLiteral("Sep_separator");
        sep3.name = QStringLiteral("Sep");
        sep3.enabled = true;
        sep3.is_separator = true;
        e3.append(sep3);
        ui::ModEntry mod3;
        mod3.id = QStringLiteral("SkyUI");
        mod3.name = QStringLiteral("SkyUI");
        mod3.enabled = true;
        e3.append(mod3);
        ui::ModEntry ow3;
        ow3.id = ui::kOverwriteModId;
        ow3.name = ui::kOverwriteModName;
        ow3.enabled = true;
        ow3.is_overwrite = true;
        e3.append(ow3);
        m3.reset_with_order(e3);

        const int sep_row = row_with_id(m3, "Sep_separator");
        const int mod_row = row_with_id(m3, "SkyUI");
        check(sep_row >= 0 && mod_row >= 0, "alignment-test rows present");

        check(Settings::instance().center_separator_text(),
              "center_separator_text defaults to on");

        // Setting on: every separator cell is centered.
        const QVariant on_name = m3.data(m3.index(sep_row, ui::ModListModel::Name),
                                         Qt::TextAlignmentRole);
        check(on_name.isValid() &&
                  on_name.toInt() == static_cast<int>(Qt::AlignCenter),
              "separator Name centered with the setting on");
        const QVariant on_prio = m3.data(m3.index(sep_row, ui::ModListModel::Priority),
                                         Qt::TextAlignmentRole);
        check(on_prio.isValid() &&
                  on_prio.toInt() == static_cast<int>(Qt::AlignCenter),
              "separator Priority centered with the setting on");

        // Setting off: separator text falls back to left alignment.
        Settings::instance().set_center_separator_text(false);
        const QVariant off_name = m3.data(m3.index(sep_row, ui::ModListModel::Name),
                                          Qt::TextAlignmentRole);
        check(!off_name.isValid(),
              "separator Name left-aligned with the setting off");
        const QVariant off_prio = m3.data(m3.index(sep_row, ui::ModListModel::Priority),
                                          Qt::TextAlignmentRole);
        check(off_prio.isValid() &&
                  off_prio.toInt() == static_cast<int>(Qt::AlignCenter),
              "separator Priority stays centered regardless");

        // Regular mods are never centered by the separator setting.
        const QVariant mod_align = m3.data(m3.index(mod_row, ui::ModListModel::Name),
                                           Qt::TextAlignmentRole);
        check(!mod_align.isValid(), "regular mod Name never centered");
        Settings::instance().set_center_separator_text(true);
    }

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
        // Column set (P8.4): exactly the 11 columns in display order. The Fold
        // column is first and carries no header label.
        check(model.columnCount() == 11, "mod list exposes 11 columns");
        const char* labels[] = {"", "Name", "Conflicts", "Flags", "Category",
                                "Source", "Source ID", "Version", "Installation",
                                "Changed", "Priority"};
        for (int c = 0; c < 11; ++c) {
            const QVariant hd = model.headerData(c, Qt::Horizontal, Qt::DisplayRole);
            if (c == ui::ModListModel::Fold) {
                check(!hd.isValid() || hd.toString().isEmpty(),
                      "Fold column has no header label");
                continue;
            }
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

    // --- Visual nesting (per-instance "Nested mod list", default off) ---
    // Mod-to-mod nesting: on-item single-source drops of the same kind become
    // children (parent_id + indentation + a fold arrow that hides the subtree).
    {
        ui::ModListModel m;
        check(!m.nesting_enabled(), "nesting gate defaults to off");
        m.set_nesting_enabled(true);
        check(m.nesting_enabled(), "nesting gate can be enabled");

        QVector<ui::ModEntry> e;
        for (const char* id : {"ModD", "ModA", "ModB", "ModC"}) {
            ui::ModEntry mod;
            mod.id = QString::fromLatin1(id);
            mod.name = mod.id;
            mod.enabled = true;
            e.append(mod);
        }
        ui::ModEntry ow;
        ow.id = ui::kOverwriteModId;
        ow.name = ui::kOverwriteModName;
        ow.enabled = true;
        ow.is_overwrite = true;
        e.append(ow);
        m.reset_with_order(e);
        // Rows: ModD(0), ModA(1), ModB(2), ModC(3), overwrite(4).

        auto rid = [&](const char* id) { return row_with_id(m, id); };
        check(m.nesting_depth(rid("ModD")) == 0, "top-level mod depth 0");

        // Nest ModB under ModA (on-item drop, valid parent index).
        QMimeData d1;
        d1.setData(QLatin1String(ui::kModListMimeType), QByteArrayLiteral("2"));
        check(m.dropMimeData(&d1, Qt::MoveAction, -1, 0, m.index(rid("ModA"), 0)),
              "mod-on-mod nest drop accepted");
        check(m.mods()[rid("ModB")].parent_id == QLatin1String("ModA"),
              "mod-on-mod drop links child to parent");
        check(m.nesting_depth(rid("ModB")) == 1, "nested mod depth 1");
        check(rid("ModB") == rid("ModA") + 1, "nested mod lands directly below its parent");
        check(m.has_content(rid("ModA")), "parent mod with a child has content");
        check(m.data(m.index(rid("ModA"), ui::ModListModel::Fold), Qt::DisplayRole)
                      .toString() == QStringLiteral("\u25BC"),
              "mod with children shows down-arrow in Fold column");

        // Grandchild: ModC under ModB.
        QMimeData d2;
        d2.setData(QLatin1String(ui::kModListMimeType), QByteArrayLiteral("3"));
        check(m.dropMimeData(&d2, Qt::MoveAction, -1, 0, m.index(rid("ModB"), 0)),
              "grandchild nest drop accepted");
        check(m.mods()[rid("ModC")].parent_id == QLatin1String("ModB"),
              "grandchild links to its immediate parent");
        check(m.nesting_depth(rid("ModC")) == 2, "grandchild depth 2");
        check(m.has_content(rid("ModB")), "intermediate mod has content");

        // Folding the root hides the whole subtree, never unrelated mods.
        m.set_folded(rid("ModA"), true);
        check(m.mods()[rid("ModA")].folded, "mod fold applies");
        check(m.is_row_fold_hidden(rid("ModB")) && m.is_row_fold_hidden(rid("ModC")),
              "folding the root hides the whole subtree");
        check(!m.is_row_fold_hidden(rid("ModD")), "unrelated mod stays visible");
        check(m.data(m.index(rid("ModA"), ui::ModListModel::Fold), Qt::DisplayRole)
                      .toString() == QStringLiteral("\u25B6"),
              "folded mod shows right-arrow in Fold column");
        m.set_folded(rid("ModA"), false);

        // Cycle guard: dragging a parent onto its own child never links.
        QMimeData dcyc;
        dcyc.setData(QLatin1String(ui::kModListMimeType), QByteArrayLiteral("1"));
        check(m.dropMimeData(&dcyc, Qt::MoveAction, -1, 0, m.index(rid("ModB"), 0)),
              "cycle drop still accepted as a move");
        check(m.mods()[rid("ModA")].parent_id.isEmpty(),
              "cycle guard: parent never nests under its own child");
        check(m.mods()[rid("ModB")].parent_id == QLatin1String("ModA"),
              "existing child link untouched by the cycle drop");

        // Subtree ride-along: dragging a parent moves its whole block, and
        // re-parenting under ModD moves the block with its children attached.
        QMimeData dsub;
        dsub.setData(QLatin1String(ui::kModListMimeType), QByteArrayLiteral("1"));
        check(m.dropMimeData(&dsub, Qt::MoveAction, -1, 0, m.index(rid("ModD"), 0)),
              "subtree re-parent drop accepted");
        check(m.mods()[rid("ModA")].parent_id == QLatin1String("ModD"),
              "block re-parented under the new target");
        check(m.mods()[rid("ModB")].parent_id == QLatin1String("ModA") &&
                  m.mods()[rid("ModC")].parent_id == QLatin1String("ModB"),
              "children keep their intra-block links");
        check(rid("ModA") == rid("ModD") + 1 && rid("ModB") == rid("ModA") + 1 &&
                  rid("ModC") == rid("ModB") + 1,
              "subtree block stays contiguous under the new parent");
        check(m.nesting_depth(rid("ModA")) == 1 && m.nesting_depth(rid("ModB")) == 2 &&
                  m.nesting_depth(rid("ModC")) == 3,
              "re-parented chain keeps increasing depth");
        check(m.has_content(rid("ModD")), "root of the re-parented block has content");
    }

    // Nest-drop ordering: repeated drops onto the same parent APPEND — each new
    // child lands after the parent's last current descendant (last child), not
    // directly below the parent (which would make it the first child and stack
    // the children in reverse drop order).
    {
        ui::ModListModel m;
        m.set_nesting_enabled(true);
        QVector<ui::ModEntry> e;
        for (const char* id : {"ModA", "ModB", "ModC"}) {
            ui::ModEntry mod;
            mod.id = QString::fromLatin1(id);
            mod.name = mod.id;
            mod.enabled = true;
            e.append(mod);
        }
        ui::ModEntry ow;
        ow.id = ui::kOverwriteModId;
        ow.name = ui::kOverwriteModName;
        ow.enabled = true;
        ow.is_overwrite = true;
        e.append(ow);
        m.reset_with_order(e);
        // Rows: ModA(0), ModB(1), ModC(2), overwrite(3).
        auto rid = [&](const char* id) { return row_with_id(m, id); };

        // First nest: no children yet, so ModB lands right below ModA.
        QMimeData d1;
        d1.setData(QLatin1String(ui::kModListMimeType), QByteArrayLiteral("1"));
        check(m.dropMimeData(&d1, Qt::MoveAction, -1, 0, m.index(rid("ModA"), 0)),
              "first nest drop accepted");
        check(rid("ModB") == rid("ModA") + 1,
              "first child lands directly below its parent");

        // Second nest: ModC must become the LAST child (after ModB), not the
        // first one squeezed in between the parent and ModB.
        QMimeData d2;
        d2.setData(QLatin1String(ui::kModListMimeType), QByteArrayLiteral("2"));
        check(m.dropMimeData(&d2, Qt::MoveAction, -1, 0, m.index(rid("ModA"), 0)),
              "second nest drop accepted");
        check(rid("ModC") == rid("ModB") + 1,
              "second child appends as the LAST child, not the first");
        check(m.nesting_depth(rid("ModB")) == 1 && m.nesting_depth(rid("ModC")) == 1,
              "both children sit at depth 1");

        // Separator variant of the same rule: dropping 02.x separators onto a
        // parent separator keeps them in drop order, not reversed.
        ui::ModListModel s;
        s.set_nesting_enabled(true);
        QVector<ui::ModEntry> se;
        for (const char* id : {"SepP_separator", "SepC_separator", "SepD_separator"}) {
            ui::ModEntry sep;
            sep.id = QString::fromLatin1(id);
            sep.name = id;
            sep.enabled = true;
            sep.is_separator = true;
            se.append(sep);
        }
        ui::ModEntry sow;
        sow.id = ui::kOverwriteModId;
        sow.name = ui::kOverwriteModName;
        sow.enabled = true;
        sow.is_overwrite = true;
        se.append(sow);
        s.reset_with_order(se);
        // Rows: SepP(0), SepC(1), SepD(2), overwrite(3).
        auto srid = [&](const char* id) { return row_with_id(s, id); };

        QMimeData sd1;
        sd1.setData(QLatin1String(ui::kModListMimeType), QByteArrayLiteral("1"));
        check(s.dropMimeData(&sd1, Qt::MoveAction, -1, 0, s.index(srid("SepP_separator"), 0)),
              "separator nest drop #1 accepted");
        QMimeData sd2;
        sd2.setData(QLatin1String(ui::kModListMimeType), QByteArrayLiteral("2"));
        check(s.dropMimeData(&sd2, Qt::MoveAction, -1, 0, s.index(srid("SepP_separator"), 0)),
              "separator nest drop #2 accepted");
        check(srid("SepD_separator") == srid("SepC_separator") + 1,
              "nested separators keep drop order (SepD last child, not first)");
        check(s.mods()[srid("SepC_separator")].parent_id == QLatin1String("SepP_separator") &&
                  s.mods()[srid("SepD_separator")].parent_id == QLatin1String("SepP_separator"),
              "both nested separators link to the parent");
    }

    // Separator nesting + nesting-aware band scope end: a folded separator hides
    // its band up to the next NON-descendant separator; nested separators and
    // their bands are swallowed by the parent's fold.
    {
        ui::ModListModel m;
        m.set_nesting_enabled(true);
        QVector<ui::ModEntry> e;
        ui::ModEntry sepP;
        sepP.id = QStringLiteral("SepP_separator");
        sepP.name = QStringLiteral("SepP");
        sepP.enabled = true;
        sepP.is_separator = true;
        e.append(sepP);
        for (const char* id : {"ModX", "ModY"}) {
            ui::ModEntry mod;
            mod.id = QString::fromLatin1(id);
            mod.name = mod.id;
            mod.enabled = true;
            e.append(mod);
        }
        ui::ModEntry sepN;
        sepN.id = QStringLiteral("SepN_separator");
        sepN.name = QStringLiteral("SepN");
        sepN.enabled = true;
        sepN.is_separator = true;
        e.append(sepN);
        ui::ModEntry sepQ;
        sepQ.id = QStringLiteral("SepQ_separator");
        sepQ.name = QStringLiteral("SepQ");
        sepQ.enabled = true;
        sepQ.is_separator = true;
        e.append(sepQ);
        ui::ModEntry modZ;
        modZ.id = QStringLiteral("ModZ");
        modZ.name = QStringLiteral("ModZ");
        modZ.enabled = true;
        e.append(modZ);
        ui::ModEntry ow;
        ow.id = ui::kOverwriteModId;
        ow.name = ui::kOverwriteModName;
        ow.enabled = true;
        ow.is_overwrite = true;
        e.append(ow);
        m.reset_with_order(e);
        // Rows: SepP(0), ModX(1), ModY(2), SepN(3), SepQ(4), ModZ(5), overwrite(6).

        auto rid = [&](const char* id) { return row_with_id(m, id); };

        // Nest SepN under SepP (separator -> separator).
        QMimeData ds;
        ds.setData(QLatin1String(ui::kModListMimeType), QByteArrayLiteral("3"));
        check(m.dropMimeData(&ds, Qt::MoveAction, -1, 0, m.index(rid("SepP_separator"), 0)),
              "separator-on-separator nest drop accepted");
        check(m.mods()[rid("SepN_separator")].parent_id == QLatin1String("SepP_separator"),
              "nested separator links to its parent separator");
        check(rid("SepN_separator") == rid("SepP_separator") + 1,
              "nested separator lands directly below its parent");
        check(m.nesting_depth(rid("SepN_separator")) == 1,
              "nested separator depth 1");
        check(m.has_content(rid("SepP_separator")),
              "separator with a nested separator child has content");

        // Rows now: SepP(0), SepN(1), ModX(2), ModY(3), SepQ(4), ModZ(5), overwrite(6).
        check(m.has_content(rid("SepN_separator")),
              "nested separator with a band below has content");

        // Fold SepN: hides ModX + ModY, stops at the non-descendant SepQ.
        m.set_folded(rid("SepN_separator"), true);
        check(m.is_row_fold_hidden(rid("ModX")) && m.is_row_fold_hidden(rid("ModY")),
              "folding a nested separator hides its band");
        check(!m.is_row_fold_hidden(rid("SepQ_separator")) &&
                  !m.is_row_fold_hidden(rid("ModZ")),
              "non-descendant separator ends the fold scope");
        check(!m.is_row_fold_hidden(rid("SepP_separator")),
              "parent separator stays visible above its child's fold");

        // Fold SepP too: swallows SepN's fold AND its band, still stops at SepQ.
        m.set_folded(rid("SepP_separator"), true);
        check(m.is_row_fold_hidden(rid("SepN_separator")) &&
                  m.is_row_fold_hidden(rid("ModX")) && m.is_row_fold_hidden(rid("ModY")),
              "folding the parent swallows the nested separator and its band");
        check(!m.is_row_fold_hidden(rid("SepQ_separator")),
              "parent fold still ends at the non-descendant separator");
        m.set_folded(rid("SepP_separator"), false);
        m.set_folded(rid("SepN_separator"), false);

        // A separator's fold scope covers mods below it regardless of nesting.
        m.set_folded(rid("SepQ_separator"), true);
        check(m.is_row_fold_hidden(rid("ModZ")),
              "folding SepQ hides the mods in its band");
        check(!m.is_row_fold_hidden(rid("ModY")),
              "SepQ fold does not reach above the non-descendant SepN");
        m.set_folded(rid("SepQ_separator"), false);
    }

    // No-link cases: kind mismatch (mod<->separator), multi-row drags, and
    // unpinnable targets (Overwrite/MERGED/game-native) never nest.
    {
        // Clean model (no native band): flush-below-separator and no-link rules.
        ui::ModListModel m;
        m.set_nesting_enabled(true);
        QVector<ui::ModEntry> e;
        ui::ModEntry sep;
        sep.id = QStringLiteral("SepS_separator");
        sep.name = QStringLiteral("SepS");
        sep.enabled = true;
        sep.is_separator = true;
        e.append(sep);
        for (const char* id : {"ModP", "ModQ"}) {
            ui::ModEntry mod;
            mod.id = QString::fromLatin1(id);
            mod.name = mod.id;
            mod.enabled = true;
            e.append(mod);
        }
        ui::ModEntry ow;
        ow.id = ui::kOverwriteModId;
        ow.name = ui::kOverwriteModName;
        ow.enabled = true;
        ow.is_overwrite = true;
        e.append(ow);
        m.reset_with_order(e);
        // Rows: SepS(0), ModP(1), ModQ(2), overwrite(3).
        auto rid = [&](const char* id) { return row_with_id(m, id); };

        // Mod dropped ON a separator: lands flush in the band, no link.
        QMimeData dmod;
        dmod.setData(QLatin1String(ui::kModListMimeType), QByteArrayLiteral("1"));
        check(m.dropMimeData(&dmod, Qt::MoveAction, -1, 0, m.index(rid("SepS_separator"), 0)),
              "mod-on-separator drop accepted");
        check(m.mods()[rid("ModP")].parent_id.isEmpty(),
              "mod dropped on a separator never nests");
        check(rid("ModP") == rid("SepS_separator") + 1,
              "mod dropped on a separator lands flush in its band");

        // Separator dropped ON a mod: no link.
        QMimeData dsep;
        dsep.setData(QLatin1String(ui::kModListMimeType), QByteArrayLiteral("0"));
        check(m.dropMimeData(&dsep, Qt::MoveAction, -1, 0, m.index(rid("ModP"), 0)),
              "separator-on-mod drop accepted");
        check(m.mods()[rid("SepS_separator")].parent_id.isEmpty(),
              "separator dropped on a mod never nests");

        // Multi-row drag onto an item: never links.
        QMimeData dmulti;
        dmulti.setData(QLatin1String(ui::kModListMimeType),
                       QByteArrayLiteral("1,2"));  // ModP + ModQ
        check(m.dropMimeData(&dmulti, Qt::MoveAction, -1, 0, m.index(rid("ModP"), 0)),
              "multi-row on-item drop accepted");
        check(m.mods()[rid("ModP")].parent_id.isEmpty() &&
                  m.mods()[rid("ModQ")].parent_id.isEmpty(),
              "multi-row drag never nests");

        // Overwrite target: never a parent.
        QMimeData dow;
        dow.setData(QLatin1String(ui::kModListMimeType), QByteArrayLiteral("1"));
        check(m.dropMimeData(&dow, Qt::MoveAction, -1, 0,
                             m.index(rid("__overwrite__"), 0)),
              "drop on Overwrite accepted");
        check(m.mods()[rid("ModP")].parent_id.isEmpty(),
              "Overwrite never becomes a parent");

        // Gate-off: preserved links become inert - no depth, no arrow, no fold
        // action, and drops never create links; re-enabling restores it all.
        QMimeData dlink;
        dlink.setData(QLatin1String(ui::kModListMimeType),
                      QByteArray::number(rid("ModQ")));
        check(m.dropMimeData(&dlink, Qt::MoveAction, -1, 0, m.index(rid("ModP"), 0)),
              "link-creating drop accepted while on");
        check(m.mods()[rid("ModQ")].parent_id == QLatin1String("ModP"),
              "gate-on drop links the mod");
        const QString preserved = m.mods()[rid("ModQ")].parent_id;
        m.set_nesting_enabled(false);
        check(m.nesting_depth(rid("ModQ")) == 0,
              "gate-off drops indentation to 0");
        check(!m.has_content(rid("ModP")),
              "gate-off hides mod content/arrows");
        check(m.data(m.index(rid("ModP"), ui::ModListModel::Fold), Qt::DisplayRole)
                      .toString().isEmpty(),
              "gate-off shows no fold arrow on a mod with preserved children");
        m.set_folded(rid("ModP"), true);
        check(m.mods()[rid("ModP")].folded,
              "gate-off fold action preserves the flag (inert)");
        check(!m.is_row_fold_hidden(rid("ModQ")),
              "gate-off fold never hides the preserved subtree");
        check(m.mods()[rid("ModQ")].parent_id == preserved,
              "gate-off preserves the parent_id links (inert, not cleared)");
        QMimeData dgateoff;
        dgateoff.setData(QLatin1String(ui::kModListMimeType),
                         QByteArray::number(rid("ModP")));
        check(m.dropMimeData(&dgateoff, Qt::MoveAction, -1, 0, m.index(rid("ModQ"), 0)),
              "gate-off drop accepted");
        check(m.mods()[rid("ModP")].parent_id.isEmpty(),
              "gate-off drop never creates a NEW link");
        check(m.mods()[rid("ModQ")].parent_id == preserved,
              "gate-off keeps the preserved link untouched");
        m.set_nesting_enabled(true);
        check(m.nesting_depth(rid("ModQ")) == 1 && m.has_content(rid("ModP")),
              "re-enabling restores depth and content");
    }

    // Game-native target (with a native band present): never a parent, and the
    // drop clamps below the band as usual.
    {
        ui::ModListModel m;
        m.set_nesting_enabled(true);
        QVector<ui::ModEntry> e;
        ui::ModEntry nat;
        nat.id = QStringLiteral("Native.esm");
        nat.name = QStringLiteral("Native.esm");
        nat.enabled = true;
        nat.is_game_native = true;
        e.append(nat);
        ui::ModEntry mod;
        mod.id = QStringLiteral("ModQ");
        mod.name = QStringLiteral("ModQ");
        mod.enabled = true;
        e.append(mod);
        ui::ModEntry ow;
        ow.id = ui::kOverwriteModId;
        ow.name = ui::kOverwriteModName;
        ow.enabled = true;
        ow.is_overwrite = true;
        e.append(ow);
        m.reset_with_order(e);
        QMimeData dnat;
        dnat.setData(QLatin1String(ui::kModListMimeType), QByteArrayLiteral("1"));
        check(m.dropMimeData(&dnat, Qt::MoveAction, -1, 0, m.index(0, 0)),
              "drop on a game-native row accepted");
        check(m.mods()[row_with_id(m, "ModQ")].parent_id.isEmpty(),
              "game-native row never becomes a parent");
        check(m.native_band_last() == 0,
              "native band intact after the drop");
    }

    // sanitize_parent_links() at load: dangling, kind-mismatched, self-linked,
    // cycled, overwrite/native-parented and missing links are all cleared;
    // valid chains survive.
    {
        ui::ModListModel m;
        QVector<ui::ModEntry> e;
        for (const char* id : {"ModA", "ModB", "ModC", "ModD", "ModE", "ModF",
                               "ModG", "ModH", "ModI", "ModJ"}) {
            ui::ModEntry mod;
            mod.id = QString::fromLatin1(id);
            mod.name = mod.id;
            mod.enabled = true;
            e.append(mod);
        }
        e[1].parent_id = QStringLiteral("ModA");          // valid child
        e[2].parent_id = QStringLiteral("ModB");          // valid grandchild
        e[3].parent_id = QStringLiteral("ghost");         // dangling
        e[4].parent_id = QStringLiteral("SepS_separator");// mod under separator
        e[5].parent_id = QStringLiteral("ModF");          // self-link
        e[6].parent_id = QStringLiteral("__overwrite__"); // under Overwrite
        e[7].parent_id = QStringLiteral("ModI");          // cycle A
        e[8].parent_id = QStringLiteral("ModH");          // cycle B
        e[9].parent_id = QStringLiteral("Native.esm");    // under game-native
        ui::ModEntry sep;
        sep.id = QStringLiteral("SepS_separator");
        sep.name = QStringLiteral("SepS");
        sep.enabled = true;
        sep.is_separator = true;
        e.append(sep);
        ui::ModEntry sepT;
        sepT.id = QStringLiteral("SepT_separator");
        sepT.name = QStringLiteral("SepT");
        sepT.enabled = true;
        sepT.is_separator = true;
        sepT.parent_id = QStringLiteral("ModA");          // separator under mod
        e.append(sepT);
        ui::ModEntry nat;
        nat.id = QStringLiteral("Native.esm");
        nat.name = QStringLiteral("Native.esm");
        nat.enabled = true;
        nat.is_game_native = true;
        e.append(nat);
        ui::ModEntry ow;
        ow.id = ui::kOverwriteModId;
        ow.name = ui::kOverwriteModName;
        ow.enabled = true;
        ow.is_overwrite = true;
        e.append(ow);
        m.reset_with_order(e);  // runs sanitize_parent_links

        auto pid = [&](const char* id) {
            return m.mods()[row_with_id(m, id)].parent_id;
        };
        check(pid("ModB") == QLatin1String("ModA") && pid("ModC") == QLatin1String("ModB"),
              "valid nesting chain survives sanitize");
        check(pid("ModD").isEmpty(), "dangling parent link cleared");
        check(pid("ModE").isEmpty(), "mod-under-separator link cleared");
        check(pid("ModF").isEmpty(), "self-link cleared");
        check(pid("ModG").isEmpty(), "overwrite-parented link cleared");
        check(!(pid("ModH") == QLatin1String("ModI") && pid("ModI") == QLatin1String("ModH")),
              "cycle links broken (no 2-cycle survives)");
        check(pid("ModJ").isEmpty(), "game-native-parented link cleared");
        check(pid("SepT_separator").isEmpty(), "separator-under-mod link cleared");
    }

    // Persistence/lifecycle: restore_parent_links (the instance.toml load
    // path), remove_mod subtree detach, rename_mod_in_place link cascade.
    {
        ui::ModListModel m;
        m.add_mod(QStringLiteral("ModA"), QStringLiteral("ModA"), QString());
        m.add_mod(QStringLiteral("ModB"), QStringLiteral("ModB"), QString());
        m.add_mod(QStringLiteral("ModC"), QStringLiteral("ModC"), QString());
        m.add_mod(QStringLiteral("ModD"), QStringLiteral("ModD"), QString());

        // Load-time restore from instance.toml's mod_parents.
        QHash<QString, QString> links;
        links.insert(QStringLiteral("ModB"), QStringLiteral("ModA"));
        links.insert(QStringLiteral("ModC"), QStringLiteral("ModB"));
        links.insert(QStringLiteral("ModD"), QStringLiteral("ghost"));
        m.restore_parent_links(links);
        auto pid = [&](const char* id) {
            return m.mods()[row_with_id(m, id)].parent_id;
        };
        check(pid("ModB") == QLatin1String("ModA") && pid("ModC") == QLatin1String("ModB"),
              "restore_parent_links applies valid links");
        check(pid("ModD").isEmpty(),
              "restore_parent_links sanitizes dangling links");

        // remove_mod detaches the subtree instead of cascade-deleting it.
        m.remove_mod(QStringLiteral("ModA"));
        check(pid("ModB").isEmpty(),
              "remove_mod detaches children (no orphaned link)");
        check(row_with_id(m, "ModB") >= 0 && row_with_id(m, "ModC") >= 0,
              "remove_mod keeps the detached subtree in the list");

        // rename_mod_in_place cascades id-based parent links.
        const int row = row_with_id(m, "ModC");
        m.rename_mod_in_place(row, QStringLiteral("ModC2"), QStringLiteral("ModC2"));
        check(pid("ModC2") == QLatin1String("ModB"),
              "rename cascades parent links to children");
    }

    // move_mod subtree ride-along: with nesting on, moving a parent moves its
    // whole block (children + grandchildren) contiguously even when unrelated
    // rows sit between them; gate-off degrades to the flat single-row move.
    {
        ui::ModListModel m;
        // Interleaved on purpose: block rows sit at 1 (P), 3 (C), 4 (G).
        for (const char* id : {"U", "P", "V", "C", "G"})
            m.add_mod(QString::fromLatin1(id), QString::fromLatin1(id), QString());
        QHash<QString, QString> links;
        links.insert(QStringLiteral("C"), QStringLiteral("P"));
        links.insert(QStringLiteral("G"), QStringLiteral("C"));
        m.restore_parent_links(links);
        m.set_nesting_enabled(true);

        m.move_mod(QStringLiteral("P"), 0);
        QVector<QString> order;
        for (const auto& mod : m.mods()) order.append(mod.id);
        check(order == (QVector<QString>{QStringLiteral("P"), QStringLiteral("C"),
                                          QStringLiteral("G"), QStringLiteral("U"),
                                          QStringLiteral("V"), QStringLiteral("__overwrite__")}),
              "move_mod rides the whole subtree along (contiguous block)");
        auto pid = [&](const char* id) {
            return m.mods()[row_with_id(m, id)].parent_id;
        };
        check(pid("C") == QLatin1String("P") && pid("G") == QLatin1String("C"),
              "move_mod keeps intra-block parent links intact");

        // Gate-off: the preserved links are inert, so only P moves.
        m.set_nesting_enabled(false);
        m.move_mod(QStringLiteral("P"), 3);
        order.clear();
        for (const auto& mod : m.mods()) order.append(mod.id);
        check(order == (QVector<QString>{QStringLiteral("C"), QStringLiteral("G"),
                                          QStringLiteral("U"), QStringLiteral("P"),
                                          QStringLiteral("V"), QStringLiteral("__overwrite__")}),
              "gate-off move_mod is a flat single-row move");
    }

    // IndentDelegate regression: a nested mod's Name cell must render exactly
    // ONE checkbox at the NORMAL (left) position - the name shifts right, the
    // checkbox never does, and there is no leftover duplicate. Render a depth-0
    // and a depth-1 row with EMPTY names (Name cell = checkbox only) and require
    // the two Name-cell crops to be pixel-identical: the old delegate cleared
    // HasCheckIndicator on the background pass, but QStyledItemDelegate::paint
    // re-runs initStyleOption (re-reading CheckStateRole) and drew the checkbox
    // TWICE - once at the normal spot, once shifted next to the name.
    {
        ui::ModListModel rm;
        rm.set_nesting_enabled(true);
        QVector<ui::ModEntry> re;
        for (const char* id : {"Parent", "Child"}) {
            ui::ModEntry mod;
            mod.id = QString::fromLatin1(id);
            mod.name.clear();  // empty: Name cell holds just the checkbox
            mod.enabled = true;
            re.append(mod);
        }
        ui::ModEntry row2;
        row2.id = ui::kOverwriteModId;
        row2.name = ui::kOverwriteModName;
        row2.enabled = true;
        row2.is_overwrite = true;
        re.append(row2);
        rm.reset_with_order(re);
        ui::ModTableView rview;
        rview.setModel(&rm);
        rview.setAlternatingRowColors(false);
        QMimeData rd;
        rd.setData(QLatin1String(ui::kModListMimeType), QByteArrayLiteral("1"));
        rm.dropMimeData(&rd, Qt::MoveAction, -1, 0,
                        rm.index(row_with_id(rm, "Parent"), 0));
        check(rm.nesting_depth(row_with_id(rm, "Child")) == 1,
              "nested child renders at depth 1");
        rview.resize(480, 140);
        rview.show();
        rview.setCurrentIndex(QModelIndex());  // no focus ring on row 0
        rview.clearFocus();
        QCoreApplication::processEvents();
        const QImage shot = rview.viewport()->grab().toImage();
        const QRect cell0 =
            rview.visualRect(rm.index(row_with_id(rm, "Parent"), ui::ModListModel::Name));
        const QRect cell1 =
            rview.visualRect(rm.index(row_with_id(rm, "Child"), ui::ModListModel::Name));
        if (cell0.isValid() && cell1.isValid() && !shot.isNull()) {
            const QImage c0 = shot.copy(cell0);
            const QImage c1 = shot.copy(cell1);
            check(c0 == c1,
                  "nested Name cell draws exactly one checkbox at the normal position");
        } else {
            check(false, "nested Name cell render geometry valid");
        }
    }

    // IndentDelegate regression: a nested SEPARATOR must indent clearly at
    // EVERY level, not just from ~depth 4. Separator names are CENTERED (the
    // "Center text on separators" default) and centered text inside a rect
    // whose left edge shifts by `shift` moves its visual center by only
    // shift/2 - so depth 1 landed at 7px (indistinguishable from a flat row),
    // depth 4 at 28px, which is why shallow nesting looked flat. The delegate
    // doubles the shift for centered rows so each level moves a full
    // kIndentStep. Render a 5-deep separator chain and require the Name text
    // to start clearly further right at each successive depth.
    {
        const bool old_center = Settings::instance().center_separator_text();
        Settings::instance().set_center_separator_text(true);
        ui::ModListModel sm;
        sm.set_nesting_enabled(true);
        QVector<ui::ModEntry> se;
        for (const char* id : {"A", "B", "C", "D", "E"}) {
            ui::ModEntry sep;
            sep.id = QString::fromLatin1(id);
            sep.name = QStringLiteral("Separator %1").arg(QString::fromLatin1(id));
            sep.enabled = true;
            sep.is_separator = true;
            sep.separator_color = "#888888";
            se.append(sep);
        }
        ui::ModEntry sow;
        sow.id = ui::kOverwriteModId;
        sow.name = ui::kOverwriteModName;
        sow.enabled = true;
        sow.is_overwrite = true;
        se.append(sow);
        sm.reset_with_order(se);
        auto nest_onto = [&](const char* child, const char* parent) {
            QMimeData d;
            d.setData(QLatin1String(ui::kModListMimeType),
                      QByteArray::number(row_with_id(sm, child)));
            sm.dropMimeData(&d, Qt::MoveAction, -1, 0,
                            sm.index(row_with_id(sm, parent), 0));
        };
        nest_onto("B", "A");
        nest_onto("C", "B");
        nest_onto("D", "C");
        nest_onto("E", "D");
        check(sm.nesting_depth(row_with_id(sm, "E")) == 4,
              "5-deep separator chain nests to depth 4");
        ui::ModTableView sv;
        sv.setModel(&sm);
        sv.setAlternatingRowColors(false);
        sv.setColumnWidth(ui::ModListModel::Name, 260);
        sv.resize(700, 180);
        sv.show();
        sv.setCurrentIndex(QModelIndex());
        sv.clearFocus();
        QCoreApplication::processEvents();
        const QImage sshot = sv.viewport()->grab().toImage();
        auto first_content_x = [&](const QImage& cell) -> int {
            const QRgb bg = cell.pixel(0, 0);
            const int y0 = cell.height() / 3;
            const int y1 = (2 * cell.height()) / 3;
            for (int x = 0; x < cell.width(); ++x)
                for (int y = y0; y <= y1; ++y) {
                    const QRgb p = cell.pixel(x, y);
                    if (qAbs(qRed(p) - qRed(bg)) > 40 ||
                        qAbs(qGreen(p) - qGreen(bg)) > 40 ||
                        qAbs(qBlue(p) - qBlue(bg)) > 40)
                        return x;
                }
            return -1;
        };
        QVector<int> starts;
        bool geom_ok = !sshot.isNull();
        for (const char* id : {"A", "B", "C", "D", "E"}) {
            const QRect cell = sv.visualRect(sm.index(row_with_id(sm, id),
                                                      ui::ModListModel::Name));
            if (!cell.isValid()) {
                geom_ok = false;
                break;
            }
            const int sx = first_content_x(sshot.copy(cell));
            if (sx < 0) {
                geom_ok = false;
                break;
            }
            starts.append(sx);
        }
        if (geom_ok && starts.size() == 5) {
            bool increasing = true;
            bool clear_step = true;
            for (int i = 1; i < starts.size(); ++i) {
                if (starts[i] <= starts[i - 1]) increasing = false;
                if (starts[i] - starts[i - 1] < 14) clear_step = false;
            }
            check(increasing,
                  "nested separator text starts further right at every depth");
            check(clear_step,
                  "each nesting depth indents the separator name by a clear, "
                  "uniform step (centered-text shift doubling)");
        } else {
            check(false, "separator indent render geometry valid");
        }
        Settings::instance().set_center_separator_text(old_center);
    }

    // IndentDelegate regression: the FIRST nested MOD must step a full
    // kIndentStep right of its parent's TEXT. Mods are left-aligned WITH a
    // checkbox, and the old shift = max(depth*kIndentStep, checkbox-width)
    // degenerated at depth 1 to EXACTLY the checkbox width - so the child's
    // suppressed-checkbox text started right where the parent's (already
    // checkbox-cleared) text starts: the first nested mod rendered ~0px past
    // its parent ("mod 2 is 2-3px left of its parent"). The shift now ADDS the
    // checkbox width, so the child lands a full step right of the parent.
    {
        ui::ModListModel mm;
        mm.set_nesting_enabled(true);
        QVector<ui::ModEntry> me;
        for (const char* id : {"P", "C"}) {
            ui::ModEntry mod;
            mod.id = QString::fromLatin1(id);
            mod.name = QStringLiteral("Mod %1").arg(QString::fromLatin1(id));
            mod.enabled = true;
            me.append(mod);
        }
        ui::ModEntry mw;
        mw.id = ui::kOverwriteModId;
        mw.name = ui::kOverwriteModName;
        mw.enabled = true;
        mw.is_overwrite = true;
        me.append(mw);
        mm.reset_with_order(me);
        QMimeData md;
        md.setData(QLatin1String(ui::kModListMimeType),
                   QByteArray::number(row_with_id(mm, "C")));
        mm.dropMimeData(&md, Qt::MoveAction, -1, 0,
                        mm.index(row_with_id(mm, "P"), 0));
        check(mm.nesting_depth(row_with_id(mm, "C")) == 1,
              "nested mod renders at depth 1");
        ui::ModTableView mv;
        mv.setModel(&mm);
        mv.setAlternatingRowColors(false);
        mv.setColumnWidth(ui::ModListModel::Name, 260);
        mv.resize(700, 120);
        mv.show();
        mv.setCurrentIndex(QModelIndex());
        mv.clearFocus();
        QCoreApplication::processEvents();
        const QImage mshot = mv.viewport()->grab().toImage();
        const int indW = QApplication::style()->pixelMetric(QStyle::PM_IndicatorWidth);
        auto text_start = [&](const QImage& cell) -> int {
            const QRgb bg = cell.pixel(0, 0);
            auto content = [&](int x) {
                for (int y = cell.height() / 3; y <= (2 * cell.height()) / 3; ++y) {
                    const QRgb p = cell.pixel(x, y);
                    if (qAbs(qRed(p) - qRed(bg)) > 40 ||
                        qAbs(qGreen(p) - qGreen(bg)) > 40 ||
                        qAbs(qBlue(p) - qBlue(bg)) > 40)
                        return true;
                }
                return false;
            };
            int cb_left = -1;
            for (int x = 0; x < cell.width(); ++x)
                if (content(x)) { cb_left = x; break; }
            if (cb_left < 0) return -1;
            for (int x = cb_left + indW + 2; x < cell.width(); ++x)
                if (content(x)) return x;
            return -1;
        };
        const QRect pcell =
            mv.visualRect(mm.index(row_with_id(mm, "P"), ui::ModListModel::Name));
        const QRect ccell =
            mv.visualRect(mm.index(row_with_id(mm, "C"), ui::ModListModel::Name));
        const int ptl = text_start(mshot.copy(pcell));
        const int ctl = text_start(mshot.copy(ccell));
        if (ptl >= 0 && ctl >= 0)
            check(ctl - ptl >= 14,
                  "first nested mod's text steps a full kIndentStep right of "
                  "its parent's text (checkbox width added, not maxed)");
        else
            check(false, "nested mod indent render geometry valid");
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
