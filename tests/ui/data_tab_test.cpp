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
//     path of the winning copy; an .exe emits execute_requested(path, true,
//     vfs_path); a native executable (exec bit) emits execute_requested(path,
//     false, vfs_path); a directory emits nothing (no real path behind it).
//     The third argument is the merged deploy-relative (DataVfsPathRole) path
//     the receiver resolves against the overlay-launch chain,
//   - preview_item() emits preview_requested with one on-disk copy per
//     provider so the preview window can browse variants,
//   - add_file_menus() exposes the MO2 menu: Open/Execute, Preview (enabled
//     only for previewable files), Add as Executable (executables only),
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
#include "engine/core/util/fs_utils.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
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
#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char* what) {
    INFO(what);
    REQUIRE(cond);
}
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

// Data-tab population is asynchronous (DataTabBuildThread + chunked fill), so
// tests must pump the event loop until the tree materializes. The worker
// thread's finished signal and the zero-timer chunk steps both run through
// processEvents(); the registry here is tiny, so one chunk completes the tree.
static void pump_until_populated(QTreeWidget* tree, int timeout_ms = 5000) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        if (tree->topLevelItemCount() > 0) return;
    }
}

static int count_leaves(QTreeWidgetItem* parent) {
    int n = 0;
    for (int i = 0; i < parent->childCount(); ++i) {
        auto* c = parent->child(i);
        n += (c->childCount() == 0) ? 1 : count_leaves(c);
    }
    return n;
}

// Pump until the tree holds at least want_leaves leaf rows (for the chunked
// large-registry regression, where pump_until_populated would return too early,
// mid-fill).
static void pump_until_leaves(QTreeWidget* tree, int want_leaves, int timeout_ms = 30000) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        if (count_leaves(tree->invisibleRootItem()) >= want_leaves) return;
    }
}

// Pump for a fixed duration, so the queue can drain even when no observable
// condition changes (e.g. verifying a re-show does NOT rebuild: the tree stays
// populated either way, only item identity tells them apart).
static void pump_ms(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

TEST_CASE("data tab", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const std::filesystem::path cfg = "/tmp/gmm_data_tab/config";
    std::filesystem::remove_all("/tmp/gmm_data_tab");
    std::filesystem::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);

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

    // BodySlide-style content: at the mod root with no Data/ wrapper, so its
    // registry key carries no deploy-prefix segment (the user's reported case).
    std::filesystem::create_directories(mod_a / "CalienteTools" / "BodySlide");
    write_file(mod_a / "CalienteTools" / "BodySlide" / "BodySlide.exe");
    // Root-override mod (skse64-style): an internal Data/ folder, so its data
    // keys carry the deploy prefix and classify into the data view with it
    // stripped.
    const auto mod_d = mods_dir / "ModD";
    std::filesystem::create_directories(mod_d / "Data" / "Tools");
    write_file(mod_d / "Data/Tools/helper.exe");

    // Mod entries mirror what MainWindow feeds the tab.
    ui::ModEntry entry_a;
    entry_a.id = "ModA";
    entry_a.name = "Mod A";
    entry_a.priority = 2;
    ui::ModEntry entry_b;
    entry_b.id = "ModB";
    entry_b.name = "Mod B";
    entry_b.priority = 1;
    ui::ModEntry entry_d;
    entry_d.id = "ModD";
    entry_d.name = "Mod D";
    entry_d.priority = 4;
    entry_d.root_override = true;
    QVector<ui::ModEntry> mods = {entry_a, entry_b, entry_d};

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
    registry["CalienteTools/BodySlide/BodySlide.exe"] = {Owner("ModA", 2)};
    registry["Data/Tools/helper.exe"] = {Owner("ModD", 4)};

    TestDataTab tab;
    tab.show();  // visible => population runs on show (deferred-population path)
    tab.show_data(registry, mods, /*conflict_reversed=*/false, mods_dir, {},
                  /*game_root_dir=*/{}, /*mods_subpath=*/"Data",
                  /*deploy_prefix=*/"Data", /*deploy_include_mod_id=*/false);
    QTreeWidget* tree = tab.tree_widget();
    pump_until_populated(tree);

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

    // --- Identical-inputs no-op: re-showing the same registry must not rebuild
    // the tree (item identity is preserved, expansion state would survive too).
    {
        TestDataTab t;
        t.show();
        t.show_data(registry, mods, /*conflict_reversed=*/false, mods_dir, {},
                    /*game_root_dir=*/{}, /*mods_subpath=*/"Data",
                    /*deploy_prefix=*/"Data", /*deploy_include_mod_id=*/false);
        QTreeWidget* tr = t.tree_widget();
        pump_until_populated(tr);
        auto* first = find_item(tr, {"Data", "readme.txt"});
        check(first != nullptr, "first show_data populates the tree");
        t.show_data(registry, mods, /*conflict_reversed=*/false, mods_dir, {},
                    /*game_root_dir=*/{}, /*mods_subpath=*/"Data",
                    /*deploy_prefix=*/"Data", /*deploy_include_mod_id=*/false);
        pump_ms(50);  // let any spurious rebuild's chunk timers run
        auto* second = find_item(tr, {"Data", "readme.txt"});
        check(second != nullptr, "identical re-show keeps the tree populated");
        check(second == first,
              "identical inputs are a no-op: tree items are not recreated");
    }

    // --- Hidden-tab deferral: show_data must not populate while the tab is not
    // visible (the RightPanel hides non-current pages; the heavy stat pass only
    // runs once the tab is actually shown).
    {
        TestDataTab t;
        // Deliberately NOT shown yet.
        t.show_data(registry, mods, /*conflict_reversed=*/false, mods_dir, {},
                    /*game_root_dir=*/{}, /*mods_subpath=*/"Data",
                    /*deploy_prefix=*/"Data", /*deploy_include_mod_id=*/false);
        QTreeWidget* tr = t.tree_widget();
        check(tr->topLevelItemCount() == 0,
              "show_data on a hidden tab defers population");
        pump_ms(50);
        check(tr->topLevelItemCount() == 0,
              "hidden tab stays empty while the event loop runs");
        t.show();
        pump_until_populated(tr);
        check(tr->topLevelItemCount() > 0 &&
                  find_item(tr, {"Data", "readme.txt"}) != nullptr,
              "showing the tab triggers the deferred population");
    }

    // --- Chunked fill: a large registry (~1500 rows, more than one 1000-row
    // chunk) must materialize completely via the progressive fill path.
    {
        constexpr int kDirs = 5;
        constexpr int kPerDir = 300;
        Registry big;
        for (int d = 0; d < kDirs; ++d) {
            for (int f = 0; f < kPerDir; ++f) {
                big["Data/bigdir" + std::to_string(d) + "/file" +
                    std::to_string(f) + ".txt"] = {Owner("ModA", 2)};
            }
        }
        TestDataTab t;
        t.show();
        t.show_data(big, mods, /*conflict_reversed=*/false, mods_dir, {},
                    /*game_root_dir=*/{}, /*mods_subpath=*/"Data",
                    /*deploy_prefix=*/"Data", /*deploy_include_mod_id=*/false);
        QTreeWidget* tr = t.tree_widget();
        pump_until_leaves(tr, kDirs * kPerDir);
        check(count_leaves(tr->invisibleRootItem()) == kDirs * kPerDir,
              "large registry materializes every file through chunked inserts");
        check(find_item(tr, {"Data", "bigdir2", "file123.txt"}) != nullptr,
              "a deep row of the large registry appears at its path");
        check(find_item(tr, {"Data", "bigdir4", "file299.txt"}) != nullptr,
              "the last chunk's last row appears at its path");
    }

    // --- Double-click dispatches.
    bool got_open = false;
    QString open_path;
    QObject::connect(&tab, &ui::DataTab::open_requested,
                     [&](const QString& p) { got_open = true; open_path = p; });
    bool got_execute = false;
    QString exec_path;
    bool exec_is_exe = false;
    QString exec_vfs;
    QObject::connect(&tab, &ui::DataTab::execute_requested,
                     [&](const QString& p, bool is_exe, const QString& vfs) {
                         got_execute = true; exec_path = p; exec_is_exe = is_exe;
                         exec_vfs = vfs;
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
                  exec_path.endsWith("test.exe") &&
                  exec_vfs == "Data/Data/scripts/test.exe",
              "double-click on an .exe emits execute_requested(..., true, vfs_path)");
    }

    got_execute = false;
    auto* tool_item = find_item(tree, {"Data", "tool.sh"});
    if (tool_item) {
        tab.on_item_double_clicked(tool_item, 0);
        check(got_execute && !exec_is_exe &&
                  exec_path.endsWith("tool.sh") &&
                  exec_vfs == "Data/Data/tool.sh",
              "double-click on a native executable emits execute_requested(..., false, vfs_path)");
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
        auto* add_exe_act = action_with_text(menu, "&Add as Executable");
        auto* reveal_act = action_with_text(menu, "Reveal in E&xplorer");
        auto* mod_info_act = action_with_text(menu, "Open &Mod Info");
        auto* hide_act = action_with_text(menu, "&Hide");
        check(open_act && open_act->isEnabled(),
              "text file menu has an enabled Open");
        check(preview_act && preview_act->isEnabled(),
              "previewable file menu has an enabled Preview");
        check(action_with_text(menu, "Open with &VFS") == nullptr,
              "no separate VFS entry: plain Execute/Open already carries the merged path");
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
        check(action_with_text(menu, "Execute with &VFS") == nullptr,
              "no separate VFS entry on executables: Execute launches in the merged view");
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
        // This fixture feeds `Data/...` keys for non-root-override mods (an
        // un-normalized layout): deploy maps them to <prefix>/Data/... so the
        // merged path doubles the prefix. Real installs peel a lone Data/
        // wrapper (normalize_staging_root), which is why the BodySlide-style
        // key below is prefix-free.
        check(got_add_exe && add_exe_path == "Data/Data/scripts/test.exe" &&
                  add_exe_name == "test.exe",
              "Add as Executable carries the deploy-relative path and file name");

        // BodySlide-style row (prefix-free key, non-root-override): the merged
        // path must gain the deploy prefix - launch resolves
        // game_dir/Data/CalienteTools/... (the real staging target), never the
        // data-view-relative CalienteTools/...
        auto* bodyslide_item = find_item(
            tree, {"CalienteTools", "BodySlide", "BodySlide.exe"});
        check(bodyslide_item != nullptr,
              "root-content file appears in the data view");
        bool got_body = false;
        QString body_path;
        QString body_phys;
        QObject::connect(&tab, &ui::DataTab::add_executable_requested,
                         [&](const QString& p, const QString& name, const QString& phys) {
                             got_body = true; body_path = p; body_phys = phys;
                         });
        if (bodyslide_item) {
            QMenu bmenu;
            tab.add_file_menus(bmenu, bodyslide_item);
            auto* body_act = action_with_text(bmenu, "&Add as Executable");
            check(body_act && body_act->isEnabled(),
                  "BodySlide row offers Add as Executable");
            if (body_act) body_act->trigger();
        }
        check(got_body &&
                  body_path == "Data/CalienteTools/BodySlide/BodySlide.exe",
              "Add as Executable prefixes root-content with the deploy dir");
        // physical_path = the winning mod's on-disk copy (icon extraction
        // source) - it must ride along even though the merged path has no
        // physical file.
        check(got_body && body_phys.endsWith("CalienteTools/BodySlide/BodySlide.exe") &&
                  std::filesystem::exists(body_phys.toStdString()),
              "Add as Executable carries the physical winning copy for icons");

        // Root-override mod's data content (skse64-style): classify stripped
        // the leading Data/ from the display path, so the merged path must add
        // it back - and no mod-folder segment, since root-override content
        // deploys straight under the prefix.
        auto* helper_item = find_item(tree, {"Tools", "helper.exe"});
        check(helper_item != nullptr,
              "root-override data content appears in the data view");
        if (helper_item) {
            QMenu hmenu;
            tab.add_file_menus(hmenu, helper_item);
            auto* helper_act = action_with_text(hmenu, "&Add as Executable");
            check(helper_act && helper_act->isEnabled(),
                  "root-override data row offers Add as Executable");
            bool got_helper = false;
            QString helper_path;
            QObject::connect(&tab, &ui::DataTab::add_executable_requested,
                             [&](const QString& p, const QString& name) {
                                 got_helper = true; helper_path = p;
                             });
            if (helper_act) helper_act->trigger();
            check(got_helper && helper_path == "Data/Tools/helper.exe",
                  "root-override data row carries the deploy-prefixed merged path");
        }
    }

    // --- Isaac-style game (deploy_include_mod_id): the merged executable path
    // carries the mod-folder segment deploy adds under <prefix>/<mod>/.
    {
        const std::filesystem::path mods2_dir = "/tmp/gmm_data_tab/mods_isaac";
        std::filesystem::create_directories(mods2_dir / "RepentanceMod" / "resources");
        write_file(mods2_dir / "RepentanceMod" / "resources/main.exe");
        ui::ModEntry entry_r;
        entry_r.id = "RepentanceMod";
        entry_r.name = "Repentance Mod";
        entry_r.priority = 7;
        QVector<ui::ModEntry> isaac_mods = {entry_r};
        Registry isaac_registry;
        isaac_registry["resources/main.exe"] = {Owner("RepentanceMod", 7)};

        TestDataTab isaac_tab;
        isaac_tab.show();
        isaac_tab.show_data(isaac_registry, isaac_mods, /*conflict_reversed=*/false,
                            mods2_dir, {}, /*game_root_dir=*/{},
                            /*mods_subpath=*/"mods", /*deploy_prefix=*/"Data",
                            /*deploy_include_mod_id=*/true);
        pump_until_populated(isaac_tab.tree_widget());
        auto* isaac_exe = find_item(isaac_tab.tree_widget(), {"resources", "main.exe"});
        check(isaac_exe != nullptr, "include-mod-id row appears in the data view");
        bool got_isaac = false;
        QString isaac_path;
        QObject::connect(&isaac_tab, &ui::DataTab::add_executable_requested,
                         [&](const QString& p, const QString& name) {
                             got_isaac = true; isaac_path = p;
                         });
        if (isaac_exe) {
            QMenu imenu;
            isaac_tab.add_file_menus(imenu, isaac_exe);
            auto* isaac_act = action_with_text(imenu, "&Add as Executable");
            check(isaac_act && isaac_act->isEnabled(),
                  "include-mod-id row offers Add as Executable");
            if (isaac_act) isaac_act->trigger();
        }
        check(got_isaac && isaac_path == "Data/RepentanceMod/resources/main.exe",
              "include-mod-id merged path carries the mod-folder segment");
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
                  /*game_root_dir=*/{}, /*mods_subpath=*/"Data",
                  /*deploy_prefix=*/"Data", /*deploy_include_mod_id=*/false);

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
}
