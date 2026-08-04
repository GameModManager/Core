// Offscreen GUI regression test for the Overwrite dialogs:
//   - QueryOverwriteDialog (MO2 "Mod Exists" port): default backup checkbox
//     state, Merge/Replace/Rename/Cancel button actions.
//   - MoveToModDialog: list population, default selection, empty-list guard.
//   - SyncOverwriteDialog: per-file combos, winner-first default, targets()
//     excluding "<don't sync>", and that unowned/non-game files stay in
//     Overwrite by default.
//   - OverwriteInfoDialog: mapping-root (e.g. "Data") protection.
//
// Hermetic: XDG_CONFIG_HOME and all file trees live under /tmp; no network,
// no user config access. QT_QPA_PLATFORM=offscreen via the test property.
#include "ui/overwrite/move_to_mod_dialog.h"
#include "ui/overwrite/overwrite_info_dialog.h"
#include "ui/overwrite/query_overwrite_dialog.h"
#include "ui/overwrite/sync_overwrite_dialog.h"
#include "ui/install/install_name_dialog.h"

#include "engine/overwrite/overwrite_utils.h"
#include "engine/pipeline/pipeline.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileSystemModel>
#include <QListWidget>
#include <QMetaObject>
#include <QModelIndex>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

static int failures = 0;
static int passes = 0;
static void check(bool cond, const char* what) {
    std::printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
    if (cond)
        ++passes;
    else
        ++failures;
}

namespace {

std::filesystem::path root_dir;

void write_file(const std::filesystem::path& path, const std::string& content = "x") {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path);
    f << content;
}

// Walks the sync dialog tree and returns each file row's (filename, combo).
std::vector<std::pair<std::string, QComboBox*>> file_rows(QTreeWidget* tree) {
    std::vector<std::pair<std::string, QComboBox*>> rows;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) {
        for (int i = 0; i < item->childCount(); ++i) walk(item->child(i));
        if (auto* combo = qobject_cast<QComboBox*>(tree->itemWidget(item, 1)))
            rows.push_back({item->text(0).toStdString(), combo});
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) walk(tree->topLevelItem(i));
    return rows;
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const std::filesystem::path cfg = "/tmp/gmm_overwrite_dialogs/config";
    std::filesystem::remove_all("/tmp/gmm_overwrite_dialogs");
    std::filesystem::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

    root_dir = "/tmp/gmm_overwrite_dialogs/data";
    std::filesystem::create_directories(root_dir);

    // ---- QueryOverwriteDialog ----------------------------------------------
    {
        ui::QueryOverwriteDialog dlg("My Mod", /*default_backup=*/true);
        auto* backup = dlg.findChild<QCheckBox*>();
        check(backup != nullptr, "query dialog has a backup checkbox");
        if (backup)
            check(backup->isChecked(), "backup checkbox defaults to checked");

        QPushButton* merge = nullptr;
        QPushButton* replace = nullptr;
        QPushButton* rename = nullptr;
        QPushButton* cancel = nullptr;
        for (auto* b : dlg.findChildren<QPushButton*>()) {
            if (b->text() == "Merge") merge = b;
            if (b->text() == "Replace") replace = b;
            if (b->text() == "Rename") rename = b;
            if (b->text() == "Cancel") cancel = b;
        }
        check(merge && replace && rename && cancel, "query dialog has all four buttons");
        check(rename && rename->isDefault(), "Rename is the default button");

        dlg.findChild<QCheckBox*>()->setChecked(false);
        merge->click();
        check(dlg.action() == engine::OverwriteAction::Merge, "Merge sets Merge action");
        check(!dlg.backup(), "backup checkbox state is read through backup()");
        check(dlg.result() == QDialog::Accepted, "Merge accepts the dialog");
    }
    {
        ui::QueryOverwriteDialog dlg("My Mod", /*default_backup=*/false);
        check(!dlg.findChild<QCheckBox*>()->isChecked(),
              "backup checkbox respects default_backup=false");
        QPushButton* replace = nullptr;
        for (auto* b : dlg.findChildren<QPushButton*>()) {
            if (b->text() == "Replace") replace = b;
        }
        replace->click();
        check(dlg.action() == engine::OverwriteAction::Replace, "Replace sets Replace action");
    }
    {
        ui::QueryOverwriteDialog dlg("My Mod", true);
        QPushButton* cancel = nullptr;
        for (auto* b : dlg.findChildren<QPushButton*>()) {
            if (b->text() == "Cancel") cancel = b;
        }
        cancel->click();
        check(dlg.action() == engine::OverwriteAction::Cancel, "Cancel keeps Cancel action");
        check(dlg.result() == QDialog::Rejected, "Cancel rejects the dialog");
    }
    {
        ui::QueryOverwriteDialog dlg("My Mod", true);
        QPushButton* rename = nullptr;
        for (auto* b : dlg.findChildren<QPushButton*>()) {
            if (b->text() == "Rename") rename = b;
        }
        rename->click();
        check(dlg.action() == engine::OverwriteAction::Rename, "Rename sets Rename action");
    }

    // ---- MoveToModDialog ----------------------------------------------------
    {
        std::vector<std::pair<std::string, std::string>> mods = {
            {"SkyUI", "SkyUI"},
            {"USSEP", "Unofficial Skyrim Patch"},
        };
        ui::MoveToModDialog dlg(mods);
        auto* list = dlg.findChild<QListWidget*>();
        check(list && list->count() == 2, "move dialog lists both mods");
        check(dlg.selected_folder() == "SkyUI", "move dialog preselects the first mod");
        if (list) {
            list->setCurrentRow(1);
            check(dlg.selected_folder() == "USSEP",
                  "move dialog selection follows the list row");
        }

        ui::MoveToModDialog empty_dlg({});
        auto* empty_list = empty_dlg.findChild<QListWidget*>();
        check(empty_list && empty_list->count() == 0, "move dialog handles an empty list");
        auto* box = empty_dlg.findChild<QDialogButtonBox*>();
        check(box && !box->button(QDialogButtonBox::Ok)->isEnabled(),
              "move dialog disables OK with no mods");
    }

    // ---- SyncOverwriteDialog --------------------------------------------------
    {
        const auto ow = root_dir / "overwrite";
        const auto mods_dir = root_dir / "mods";
        const auto game_dir = root_dir / "game";

        // ModA owns Data/meshes/foo.nif, ModB owns Data/textures/bar.dds, and
        // both own Data/meshes/shared.nif (conflict). The mod folder root maps
        // onto the game's Data dir, so the mod-local file is "meshes/foo.nif"
        // (no "Data" prefix inside the mod folder).
        write_file(ow / "Data/meshes/foo.nif");
        write_file(ow / "Data/textures/bar.dds");
        write_file(ow / "Data/meshes/shared.nif");
        write_file(ow / "ControlMap_Custom.txt");  // no owner, not in the game
        write_file(mods_dir / "ModA/meshes/foo.nif");
        write_file(mods_dir / "ModB/textures/bar.dds");
        write_file(mods_dir / "ModA/meshes/shared.nif");
        write_file(mods_dir / "ModB/meshes/shared.nif");

        ui::SyncOverwriteDialog::Context ctx;
        ctx.overwrite_dir = ow;
        ctx.mods_dir = mods_dir;
        ctx.mod_infos = {{"ModA", 0}, {"ModB", 1}};
        ctx.mods_subpath = "Data";
        ctx.conflict_reversed = false;
        ctx.include_mod_id = false;
        ctx.game_dir = game_dir;
        ctx.game_folder = "Skyrim Special Edition";
        ctx.game_label = "Skyrim Special Edition";
        ctx.metadata_file = "meta.ini";

        ui::SyncOverwriteDialog dlg(ctx);
        auto* tree = dlg.findChild<QTreeWidget*>();
        auto rows = file_rows(tree);
        check(rows.size() == 4, "sync dialog shows one combo per overwrite file");

        std::string foo_owner, bar_owner, shared_owner, control_default;
        for (const auto& [name, combo] : rows) {
            if (name == "foo.nif") foo_owner = combo->currentData().toString().toStdString();
            if (name == "bar.dds") bar_owner = combo->currentData().toString().toStdString();
            if (name == "shared.nif") shared_owner = combo->currentData().toString().toStdString();
            if (name == "ControlMap_Custom.txt")
                control_default = combo->currentData().toString().toStdString();
        }
        check(foo_owner == "ModA", "foo.nif defaults to its winning owner ModA");
        check(bar_owner == "ModB", "bar.dds defaults to its winning owner ModB");
        check(shared_owner == "ModB", "conflict defaults to the higher-priority owner ModB");
        check(control_default.empty(),
              "unowned/non-game file defaults to <don't sync> (stays in Overwrite)");

        // Accept with defaults: owned files move, unowned stays.
        QMetaObject::invokeMethod(&dlg, "accept");
        auto targets = dlg.targets();
        check(targets.size() == 3, "default targets() sync only the owned files");
        bool has_foo = false, has_bar = false, has_shared = false;
        for (const auto& t : targets) {
            if (t.overwrite_rel == "Data/meshes/foo.nif" && t.mod_folder == "ModA") has_foo = true;
            if (t.overwrite_rel == "Data/textures/bar.dds" && t.mod_folder == "ModB") has_bar = true;
            if (t.overwrite_rel == "Data/meshes/shared.nif" && t.mod_folder == "ModB") has_shared = true;
        }
        check(has_foo && has_bar && has_shared, "targets() maps each owned file to its owner mod");

        // Reassign the conflicting file to the lower-priority mod.
        ui::SyncOverwriteDialog dlg2(ctx);
        auto rows2 = file_rows(dlg2.findChild<QTreeWidget*>());
        for (const auto& [name, combo] : rows2) {
            if (name == "shared.nif") {
                for (int i = 0; i < combo->count(); ++i) {
                    if (combo->itemData(i).toString() == "ModA") combo->setCurrentIndex(i);
                }
            }
        }
        QMetaObject::invokeMethod(&dlg2, "accept");
        bool moved_shared = false;
        for (const auto& t : dlg2.targets()) {
            if (t.overwrite_rel == "Data/meshes/shared.nif" && t.mod_folder == "ModA")
                moved_shared = true;
        }
        check(moved_shared, "reassigned combo moves the file into the chosen mod");
    }

    // ---- OverwriteInfoDialog ------------------------------------------------
    {
        const auto ow = root_dir / "overwrite_info";
        write_file(ow / "Data/meshes/foo.nif");
        write_file(ow / "loose.txt");

        ui::OverwriteInfoDialog dlg(ow, "Data");
        auto* model = dlg.findChild<QFileSystemModel*>();
        check(model != nullptr, "info dialog exposes a file system model");

        const auto data_idx =
            model->index(QString::fromStdString((ow / "Data").string()));
        const auto loose_idx =
            model->index(QString::fromStdString((ow / "loose.txt").string()));
        check(dlg.is_mapping_root(data_idx),
              "info dialog protects the mapping-root dir (Data)");
        check(!dlg.is_mapping_root(loose_idx),
              "info dialog does not protect ordinary files");

        // An Isaac-style game (mods_subpath="mods") must NOT protect a "Data"
        // dir, only the root itself.
        ui::OverwriteInfoDialog isaac(ow, "mods");
        auto* isaac_model = isaac.findChild<QFileSystemModel*>();
        const auto isaac_data_idx =
            isaac_model->index(QString::fromStdString((ow / "Data").string()));
        const auto isaac_root_idx =
            isaac_model->index(QString::fromStdString(ow.string()));
        check(!isaac.is_mapping_root(isaac_data_idx),
              "Isaac-style subpath does not protect a Data dir");
        check(isaac.is_mapping_root(isaac_root_idx),
              "info dialog protects the overwrite root itself");
    }

    // ---- InstallNameDialog (MO2 Quick Install port) --------------------------
    {
        // Candidates: Nexus name first, cleaned archive stem second, full
        // archive filename last; no duplicates.
        auto names = ui::InstallNameDialog::candidates(
            "SkyUI", "SkyUI_5_2_SE-38604-5-2SE-1604800124.zip");
        check(names.size() == 3, "name dialog offers exactly three candidates");
        check(names[0] == "SkyUI", "Nexus display name is the preferred default");
        check(names[1] == "SkyUI 5 2 SE", "cleaned archive stem is offered next");
        check(names[2] == "SkyUI_5_2_SE-38604-5-2SE-1604800124.zip",
              "full archive filename is offered last");

        // The full archive filename is deduplicated when it equals the guess.
        auto dups = ui::InstallNameDialog::candidates("My Mod.zip", "My Mod.zip");
        check(dups.size() == 2, "candidates deduplicate identical names");

        // Fallback when nothing resolvable is given.
        auto fallback = ui::InstallNameDialog::candidates("", "");
        check(fallback.size() == 1 && fallback[0] == "New Mod",
              "empty inputs fall back to a placeholder name");

        ui::InstallNameDialog dlg("SkyUI", "SkyUI_5_2_SE.zip");
        auto* combo = dlg.findChild<QComboBox*>();
        check(combo && combo->isEditable(), "name dialog uses an editable combobox");
        check(combo && combo->count() == 3, "name combo carries all candidates");
        check(dlg.name() == "SkyUI", "name() returns the preferred default");

        // Picking a different dropdown entry or typing overrides the default.
        if (combo) {
            combo->setCurrentIndex(1);
            check(dlg.name() == "SkyUI 5 2 SE", "name() follows the dropdown selection");
            combo->setEditText("Custom Name");
            check(dlg.name() == "Custom Name", "name() follows typed text");
        }
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
