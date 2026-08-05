// Data tab — offscreen GUI regression test for the MO2-parity file context
// menu and double-click behavior.
//
// The tab is fed a conflict registry (path -> [(mod, priority), ...]) the same
// way MainWindow does, then the test verifies:
//   - show_data() builds the tree with the winning file's name/size/source
//     and a providers count for multi-provider files,
//   - hidden files (`.gmmhidden` here, `.mohidden` in MO2-imported instances)
//     are listed under their base name and dimmed, and a visible file sharing
//     the base name claims the row over its hidden copy,
//   - double-clicking a non-executable emits open_requested with the on-disk
//     path of the winning copy; an .exe emits execute_requested(..., true);
//     a native executable (exec bit) emits execute_requested(..., false);
//     a directory emits nothing (no real path behind it),
//   - preview_item() emits preview_requested with one on-disk copy per
//     provider so the preview window can browse variants,
//   - add_file_menus() exposes the MO2 menu: Open/Execute, Preview (enabled
//     only for previewable files), the two VFS entries disabled with the
//     platform-constraints tooltip, Add as Executable (executables only),
//     Reveal in Explorer, Open Mod Info (disabled for Overwrite/MERGED),
//     Hide/Un-Hide, and the common menus (Save Tree, Refresh, Expand/Collapse),
//   - triggering the menu actions emits the right signals with the right
//     payloads (hide_requested carries the real path; add_executable_requested
//     carries the suggested name; open_mod_info_requested carries the winner
//     mod id),
//   - Overwrite-owned files show "Overwrite" as their source and have Mod Info
//     disabled.
//
// The context-menu internals are protected on DataTab so a test subclass can
// drive add_file_menus/open_item/preview_item directly (menu.exec() is modal,
// so the full on_custom_context_menu flow is not exercised - the per-file
// action wiring underneath it is).
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME and /tmp file tree.
#include "ui/panels/tab_panels.h"
#include "ui/widgets/mod_list_model.h"
#include "engine/fs_utils.h"

#include <QApplication>
#include <QMenu>
#include <QPalette>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
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

using Registry = std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>;
using Owner = std::pair<std::string, int>;

static void write_file(const std::filesystem::path& path, const char* content = "x") {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

static void make_executable(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::permissions(path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
            std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add, ec);
}

// Find a tree item by its display path, e.g. find_item(tree, {"Data", "meshes",
// "test.nif"}). Returns nullptr if any segment is missing.
static QTreeWidgetItem* find_item(QTreeWidget* tree,
                                  const std::vector<const char*>& path) {
    QTreeWidgetItem* cur = nullptr;
    for (size_t i = 0; i < path.size(); ++i) {
        QTreeWidgetItem* parent = cur ? cur : tree->invisibleRootItem();
        QTreeWidgetItem* next = nullptr;
        for (int c = 0; c < parent->childCount(); ++c) {
            auto* child = parent->child(c);
            if (child->text(0) == QLatin1String(path[i])) { next = child; break; }
        }
        if (!next) return nullptr;
        cur = next;
    }
    return cur;
}

static QAction* action_with_text(QMenu& menu, const char* text) {
    for (auto* a : menu.actions()) {
        if (a->text() == QLatin1String(text)) return a;
    }
    return nullptr;
}

// Expose the protected context-menu / open / preview internals for direct
// driving (see the header caveat above).
struct TestDataTab : ui::DataTab {
    using ui::DataTab::add_common_menus;
    using ui::DataTab::add_file_menus;
    using ui::DataTab::on_item_double_clicked;
    using ui::DataTab::open_item;
    using ui::DataTab::preview_item;
};

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const std::filesystem::path cfg = "/tmp/gmm_data_tab/config";
    std::filesystem::remove_all("/tmp/gmm_data_tab");
    std::filesystem::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    QApplication app(argc, argv);

    // --- Build two mod folders on disk. ModA wins every shared file.
    const std::filesystem::path mods_dir = "/tmp/gmm_data_tab/mods";
    const auto mod_a = mods_dir / "ModA";
    const auto mod_b = mods_dir / "ModB";
    for (const auto& sub : {std::string("Data/meshes"), std::string("Data/textures"),
                            std::string("Data/scripts")})
        std::filesystem::create_directories(mod_a / sub);
    std::filesystem::create_directories(mod_b / "Data/meshes");
    std::filesystem::create_directories(mod_b / "Data/textures");

    write_file(mod_a / "Data/meshes" / "test.nif");
    write_file(mod_b / "Data/meshes" / "test.nif");  // conflict: ModA wins
    write_file(mod_a / "Data/textures" / "albedo.png");
    write_file(mod_b / "Data/textures" / "albedo.png");  // 2 providers
    write_file(mod_a / "Data/scripts" / "test.exe");
    write_file(mod_a / "Data/readme.txt");
    const auto tool_sh = mod_a / "Data/tool.sh";
    write_file(tool_sh, "#!/bin/sh\n");
    make_executable(tool_sh);
    write_file(mod_a / "Data/foo.txt");
    write_file(mod_a / "Data/foo.txt.gmmhidden");  // hidden GMM copy
    write_file(mod_a / "Data/bar.txt.mohidden");   // hidden MO2 copy

    // Mod entries mirror what MainWindow feeds the tab.
    ui::ModEntry entry_a;
    entry_a.id = "ModA";
    entry_a.name = "Mod A";
    entry_a.priority = 2;
    ui::ModEntry entry_b;
    entry_b.id = "ModB";
    entry_b.name = "Mod B";
    entry_b.priority = 1;
    QVector<ui::ModEntry> mods = {entry_a, entry_b};

    Registry registry;
    registry["Data/meshes/test.nif"] = {Owner("ModA", 2), Owner("ModB", 1)};
    registry["Data/textures/albedo.png"] = {Owner("ModA", 2), Owner("ModB", 1)};
    registry["Data/scripts/test.exe"] = {Owner("ModA", 2)};
    registry["Data/readme.txt"] = {Owner("ModA", 2)};
    registry["Data/tool.sh"] = {Owner("ModA", 2)};
    registry["Data/foo.txt"] = {Owner("ModA", 2)};
    registry["Data/foo.txt.gmmhidden"] = {Owner("ModA", 2)};
    registry["Data/bar.txt.mohidden"] = {Owner("ModA", 2)};
    registry["Data/overwrite.txt"] = {Owner(ui::kOverwriteModId, 999999)};

    TestDataTab tab;
    tab.show_data(registry, mods, /*conflict_reversed=*/false, mods_dir, {},
                  /*game_root_dir=*/{}, /*mods_subpath=*/"Data", /*deploy_prefix=*/"Data");
    QTreeWidget* tree = tab.tree_widget();

    // --- Tree population.
    check(find_item(tree, {"Data", "meshes", "test.nif"}) != nullptr,
          "nested file appears under its directory path");
    check(find_item(tree, {"Data", "readme.txt"}) != nullptr,
          "file directly under the Data dir appears");
    auto* nif_item = find_item(tree, {"Data", "meshes", "test.nif"});
    if (nif_item) {
        check(nif_item->text(1).isEmpty() == false,
              "file row shows a size");
        check(nif_item->text(2) == "Mod A",
              "file row source shows the winning mod's display name");
        check(nif_item->text(3) == "2",
              "multi-provider file shows a providers count");
    }

    // --- Hidden files render under their base name, dimmed.
    auto* foo_item = find_item(tree, {"Data", "foo.txt"});
    check(foo_item != nullptr,
          "visible file claims the display slot over its hidden copy");
    if (foo_item) {
        check(!(foo_item->foreground(0).color() ==
                QApplication::palette().color(QPalette::Disabled, QPalette::Text)),
              "visible file is not dimmed");
    }
    auto* bar_item = find_item(tree, {"Data", "bar.txt"});
    check(bar_item != nullptr,
          "MO2-hidden file (bar.txt.mohidden) appears under its base name");
    if (bar_item) {
        check(bar_item->foreground(0).color() ==
                  QApplication::palette().color(QPalette::Disabled, QPalette::Text),
              "hidden file is dimmed");
    }
    check(find_item(tree, {"Data", "bar.txt.mohidden"}) == nullptr,
          "hidden suffix is stripped from the displayed name");

    // --- Double-click dispatches.
    bool got_open = false;
    QString open_path;
    QObject::connect(&tab, &ui::DataTab::open_requested,
                     [&](const QString& p) { got_open = true; open_path = p; });
    bool got_execute = false;
    QString exec_path;
    bool exec_is_exe = false;
    QObject::connect(&tab, &ui::DataTab::execute_requested,
                     [&](const QString& p, bool is_exe) {
                         got_execute = true; exec_path = p; exec_is_exe = is_exe;
                     });

    auto* readme_item = find_item(tree, {"Data", "readme.txt"});
    if (readme_item) {
        tab.on_item_double_clicked(readme_item, 0);
        check(got_open && open_path ==
                  QString::fromStdString((mod_a / "Data/readme.txt").string()),
              "double-click on a text file emits open_requested with the real path");
    }

    auto* exe_item = find_item(tree, {"Data", "scripts", "test.exe"});
    if (exe_item) {
        tab.on_item_double_clicked(exe_item, 0);
        check(got_execute && exec_is_exe &&
                  exec_path.endsWith("test.exe"),
              "double-click on an .exe emits execute_requested(..., true)");
    }

    got_execute = false;
    auto* tool_item = find_item(tree, {"Data", "tool.sh"});
    if (tool_item) {
        tab.on_item_double_clicked(tool_item, 0);
        check(got_execute && !exec_is_exe &&
                  exec_path.endsWith("tool.sh"),
              "double-click on a native executable emits execute_requested(..., false)");
    }

    bool got_after_dir = false;
    QObject::connect(&tab, &ui::DataTab::open_requested,
                     [&](const QString&) { got_after_dir = true; });
    auto* dir_item = find_item(tree, {"Data"});
    if (dir_item) {
        tab.on_item_double_clicked(dir_item, 0);
        check(!got_after_dir,
              "double-click on a directory emits nothing");
    }

    // --- Preview with provider variants.
    bool got_preview = false;
    QString preview_primary;
    QStringList preview_paths;
    QStringList preview_names;
    QObject::connect(&tab, &ui::DataTab::preview_requested,
                     [&](const QString& p, const QStringList& paths,
                         const QStringList& names) {
                         got_preview = true;
                         preview_primary = p;
                         preview_paths = paths;
                         preview_names = names;
                     });
    auto* albedo_item = find_item(tree, {"Data", "textures", "albedo.png"});
    if (albedo_item) {
        tab.preview_item(albedo_item);
        check(got_preview && preview_paths.size() == 2 &&
                  preview_names.size() == 2 &&
                  preview_primary == preview_paths.first() &&
                  preview_primary.endsWith("ModA/Data/textures/albedo.png") &&
                  preview_names.contains("ModA") && preview_names.contains("ModB"),
              "preview_item emits one on-disk copy per provider (variant browsing)");
    }

    // --- Context menu on a non-executable previewable file.
    {
        QMenu menu;
        tab.add_file_menus(menu, readme_item);
        auto* open_act = action_with_text(menu, "&Open");
        auto* preview_act = action_with_text(menu, "&Preview");
        auto* vfs_act = action_with_text(menu, "Open with &VFS");
        auto* add_exe_act = action_with_text(menu, "&Add as Executable");
        auto* reveal_act = action_with_text(menu, "Reveal in E&xplorer");
        auto* mod_info_act = action_with_text(menu, "Open &Mod Info");
        auto* hide_act = action_with_text(menu, "&Hide");
        check(open_act && open_act->isEnabled(),
              "text file menu has an enabled Open");
        check(preview_act && preview_act->isEnabled(),
              "previewable file menu has an enabled Preview");
        check(vfs_act && !vfs_act->isEnabled() &&
                  vfs_act->toolTip().contains("Not implemented"),
              "VFS entry is disabled with the platform-constraints hint");
        check(add_exe_act && !add_exe_act->isEnabled(),
              "Add as Executable is disabled for a non-executable");
        check(reveal_act && reveal_act->isEnabled(),
              "Reveal in Explorer is enabled for a managed file");
        check(mod_info_act && mod_info_act->isEnabled(),
              "Open Mod Info is enabled for a managed mod");
        check(hide_act && hide_act->isEnabled() && hide_act->text() == "&Hide",
              "visible file menu offers Hide");

        // Triggering the actions emits the wired signals with the right payloads.
        bool got_mod_info = false;
        QString mod_info_id;
        QObject::connect(&tab, &ui::DataTab::open_mod_info_requested,
                         [&](const QString& id) { got_mod_info = true; mod_info_id = id; });
        mod_info_act->trigger();
        check(got_mod_info && mod_info_id == "ModA",
              "Open Mod Info triggers with the winner mod id");

        bool got_hide = false;
        QString hide_path;
        QString hide_mod_id;
        bool hide_flag = false;
        QObject::connect(&tab, &ui::DataTab::hide_requested,
                         [&](const QString& p, const QString& mod, bool h) {
                             got_hide = true; hide_path = p;
                             hide_mod_id = mod; hide_flag = h;
                         });
        hide_act->trigger();
        check(got_hide && hide_flag && hide_mod_id == "ModA" &&
                  hide_path.endsWith("Data/readme.txt"),
              "Hide triggers with the real path, winner mod id, and hide=true");
    }

    // --- Context menu on an executable.
    {
        QMenu menu;
        tab.add_file_menus(menu, exe_item);
        check(action_with_text(menu, "&Execute") &&
                  action_with_text(menu, "&Execute")->isEnabled(),
              "executable menu has an enabled Execute");
        auto* preview_act = action_with_text(menu, "&Preview");
        check(preview_act && !preview_act->isEnabled(),
              "Preview is disabled for a non-previewable .exe");
        auto* vfs_act = action_with_text(menu, "Execute with &VFS");
        check(vfs_act && !vfs_act->isEnabled(),
              "Execute with VFS is disabled");
        auto* add_exe_act = action_with_text(menu, "&Add as Executable");
        check(add_exe_act && add_exe_act->isEnabled(),
              "Add as Executable is enabled for an executable");

        // Triggering it emits the merged-view (deploy-relative) path - what
        // the launch overlay resolves - never the on-disk mods-folder path.
        bool got_add_exe = false;
        QString add_exe_path;
        QString add_exe_name;
        QObject::connect(&tab, &ui::DataTab::add_executable_requested,
                         [&](const QString& p, const QString& name) {
                             got_add_exe = true; add_exe_path = p; add_exe_name = name;
                         });
        if (add_exe_act) add_exe_act->trigger();
        check(got_add_exe && add_exe_path == "Data/scripts/test.exe" &&
                  add_exe_name == "test.exe",
              "Add as Executable carries the deploy-relative path and file name");
    }

    // --- Context menu on a hidden file offers Un-Hide.
    {
        QMenu menu;
        tab.add_file_menus(menu, bar_item);
        check(action_with_text(menu, "&Un-Hide") != nullptr,
              "hidden file menu offers Un-Hide");
    }

    // --- Overwrite-owned file: Overwrite source, Mod Info disabled.
    auto* overwrite_item = find_item(tree, {"Data", "overwrite.txt"});
    check(overwrite_item != nullptr, "Overwrite file appears in the tree");
    if (overwrite_item) {
        check(overwrite_item->text(2) == "Overwrite",
              "Overwrite file source column reads Overwrite");
        QMenu menu;
        tab.add_file_menus(menu, overwrite_item);
        auto* mod_info_act = action_with_text(menu, "Open &Mod Info");
        check(mod_info_act && !mod_info_act->isEnabled(),
              "Open Mod Info is disabled for Overwrite files");
    }

    // --- Common menus are present for any context.
    {
        QMenu menu;
        tab.add_common_menus(menu);
        check(action_with_text(menu, "&Save Tree to Text File...") != nullptr &&
                  action_with_text(menu, "&Refresh") != nullptr &&
                  action_with_text(menu, "Ex&pand All") != nullptr &&
                  action_with_text(menu, "&Collapse All") != nullptr,
              "common menus expose Save Tree / Refresh / Expand / Collapse");
    }

    // --- Incremental apply_mod(): merge one freshly installed mod (ModC) into
    // the existing tree instead of rebuilding it. ModC lands at the bottom of
    // the user band, i.e. the highest priority, so it wins every file it
    // shares (mirrors what add_mod() + renumber_priorities() do on install).
    const auto mod_c = mods_dir / "ModC";
    std::filesystem::create_directories(mod_c / "Data" / "meshes");
    write_file(mod_c / "Data/meshes" / "test.nif");    // now shared; ModC wins
    write_file(mod_c / "Data" / "newfile.txt");        // sole ModC file
    write_file(mod_c / "Data" / "foo.txt.gmmhidden");  // hidden copy of a live row

    ui::ModEntry entry_c;
    entry_c.id = "ModC";
    entry_c.name = "Mod C";
    entry_c.priority = 3;
    mods.push_back(entry_c);

    // The fresh registry the engine produced after ModC was installed.
    Registry registry2;
    registry2["Data/meshes/test.nif"] = {Owner("ModA", 2), Owner("ModB", 1), Owner("ModC", 3)};
    registry2["Data/textures/albedo.png"] = {Owner("ModA", 2), Owner("ModB", 1), Owner("ModC", 3)};
    registry2["Data/scripts/test.exe"] = {Owner("ModA", 2)};
    registry2["Data/readme.txt"] = {Owner("ModA", 2)};
    registry2["Data/tool.sh"] = {Owner("ModA", 2)};
    registry2["Data/foo.txt"] = {Owner("ModA", 2)};
    registry2["Data/foo.txt.gmmhidden"] = {Owner("ModA", 2), Owner("ModC", 3)};
    registry2["Data/bar.txt.mohidden"] = {Owner("ModA", 2)};
    registry2["Data/overwrite.txt"] = {Owner(ui::kOverwriteModId, 999999)};
    registry2["Data/newfile.txt"] = {Owner("ModC", 3)};

    auto* nif_before = find_item(tree, {"Data", "meshes", "test.nif"});
    tab.apply_mod(registry2, "ModC", mods, /*conflict_reversed=*/false, mods_dir, {},
                  /*game_root_dir=*/{}, /*mods_subpath=*/"Data", /*deploy_prefix=*/"Data");

    auto* nif_after = find_item(tree, {"Data", "meshes", "test.nif"});
    check(nif_after != nullptr, "shared file row survives the incremental update");
    check(nif_after == nif_before,
          "shared file row is updated in place, not recreated");
    if (nif_after) {
        check(nif_after->text(2) == "Mod C",
              "winner flips to the newly installed mod (bottom of the band)");
        check(nif_after->text(3) == "3",
              "provider count bumped 2 -> 3 for a file the new mod provides");
    }
    auto* newfile_item = find_item(tree, {"Data", "newfile.txt"});
    check(newfile_item != nullptr,
          "sole-owned file of the installed mod inserts a new row");
    if (newfile_item) {
        check(newfile_item->text(2) == "Mod C" && newfile_item->text(3).isEmpty(),
              "sole-owned file shows the new mod as source and no provider count");
    }
    auto* readme_after = find_item(tree, {"Data", "readme.txt"});
    check(readme_after != nullptr && readme_after->text(2) == "Mod A",
          "rows the new mod does not touch are left alone");
    auto* foo_after = find_item(tree, {"Data", "foo.txt"});
    check(foo_after != nullptr &&
              !(foo_after->foreground(0).color() ==
                QApplication::palette().color(QPalette::Disabled, QPalette::Text)),
          "hidden copy of an existing row does not reclaim its display slot");

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
