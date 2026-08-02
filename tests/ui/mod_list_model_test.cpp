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
#include "ui/settings/settings.h"

#include <QApplication>
#include <QBrush>
#include <QMimeData>
#include <QModelIndexList>

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

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
