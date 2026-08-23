// FiletreeTab — offscreen GUI regression test (Workspace-2d2).
//
// set_mod() is the only refresh hook a tab gets when navigating mods with the
// dialog's prev/next arrows (first_activation() runs once per dialog session).
// This pins that set_mod() re-roots the tree at the new mod's directory —
// previously the wiring lived in first_activation(), so arrow navigation left
// the tree showing the previous mod's content.
//
// Hermetic: offscreen platform, throwaway /tmp tree.
#include "ui/modinfo/filetree_tab.h"

#include <QApplication>
#include <QFileSystemModel>
#include <QTreeView>

#include <filesystem>
#include <fstream>
#include <string>
#include <catch2/catch_test_macros.hpp>

static void write_file(const std::filesystem::path& path, const char* content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

TEST_CASE("filetree tab re-roots on set_mod", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const std::filesystem::path base = "/tmp/gmm_filetree_tab";
    std::filesystem::remove_all(base);
    const std::filesystem::path mod_a = base / "ModA";
    const std::filesystem::path mod_b = base / "ModB";
    write_file(mod_a / "fileA.txt", "a\n");
    write_file(mod_b / "fileB.txt", "b\n");
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);

    ui::ModInfoData data_a;
    data_a.id = "ModA";
    data_a.mod_dir = QDir(QString::fromStdString(mod_a.string()));
    ui::ModInfoData data_b;
    data_b.id = "ModB";
    data_b.mod_dir = QDir(QString::fromStdString(mod_b.string()));

    ui::FiletreeTab tab;
    auto* model = tab.findChild<QFileSystemModel*>();
    auto* tree = tab.findChild<QTreeView*>();
    REQUIRE(model);
    REQUIRE(tree);

    // Dialog lifecycle: initial visit wires the view, then navigation calls
    // only set_mod() — first_activation() never runs again.
    tab.set_mod(data_a);
    tab.first_activation();
    REQUIRE(model->rootPath() == data_a.mod_dir.absolutePath());

    // Navigate to ModB via arrows: the tree must follow.
    tab.set_mod(data_b);
    CHECK(model->rootPath() == data_b.mod_dir.absolutePath());
    REQUIRE(tree->rootIndex().isValid());
    CHECK(tree->rootIndex() == model->index(data_b.mod_dir.absolutePath()));
}
