// Offscreen GUI test for the MO2-style CategoriesDialog.
//
// Covers: table populated from engine::CategoryFactory (ID | Name | ParentID),
// Add inserting a row with the next free ID, Remove deleting the selected row,
// ParentID validation (0 or an existing id, never self), duplicate-ID
// rejection, and commit_changes applying the table to the factory as a diff
// (add/update/remove) plus persisting categories.dat.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, no network.
#include "ui/settings/categories_dialog.h"
#include "engine/pipeline/plugin_host/category_factory.h"

#include <QApplication>
#include <QPushButton>
#include <QTableWidget>

#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>

namespace {

// Resets the global factory to a known state (the test binary starts with an
// empty registry, but tests may run in any order). Ids are collected before
// removal — removeCategory erases from the map, so iterating while removing
// would invalidate the iterator.
void seed_factory() {
    auto& factory = engine::CategoryFactory::instance();
    std::vector<int> ids;
    for (const auto& [id, cat] : factory.categories()) {
        Q_UNUSED(cat)
        if (id != 0)
            ids.push_back(id);
    }
    for (int id : ids)
        factory.removeCategory(id);
    factory.addCategory(1, "Animations", 0);
    factory.addCategory(2, "Armour", 0);
    factory.addCategory(3, "Poses", 1);  // child of Animations
}

QTableWidget* table_of(ui::CategoriesDialog& dlg) {
    auto* table = dlg.findChild<QTableWidget*>();
    REQUIRE(table != nullptr);
    return table;
}

QPushButton* button_of(ui::CategoriesDialog& dlg, const QString& text) {
    for (auto* btn : dlg.findChildren<QPushButton*>()) {
        if (btn->text() == text)
            return btn;
    }
    return nullptr;
}

// Reads a cell as text (works for both int-stored and string-stored items).
QString cell_text(QTableWidget* table, int row, int column) {
    auto* item = table->item(row, column);
    return item ? item->text() : QString();
}

}  // namespace

TEST_CASE("categories dialog", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const std::filesystem::path root = "/tmp/gmm_categories_dialog";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "config");
    qputenv("XDG_CONFIG_HOME", (root / "config").c_str());
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

    seed_factory();

    SECTION("table mirrors the factory") {
        ui::CategoriesDialog dlg(root);
        auto* table = table_of(dlg);
        REQUIRE(table->columnCount() == 3);
        REQUIRE(table->rowCount() == 3);
        // Rows are sorted by ID (sorting enabled), so order is 1, 2, 3.
        REQUIRE(cell_text(table, 0, 0) == "1");
        REQUIRE(cell_text(table, 0, 1) == "Animations");
        REQUIRE(cell_text(table, 0, 2) == "0");
        REQUIRE(cell_text(table, 1, 0) == "2");
        REQUIRE(cell_text(table, 1, 1) == "Armour");
        REQUIRE(cell_text(table, 2, 0) == "3");
        REQUIRE(cell_text(table, 2, 1) == "Poses");
        REQUIRE(cell_text(table, 2, 2) == "1");
    }

    SECTION("add inserts a row with the next free id and root parent") {
        ui::CategoriesDialog dlg(root);
        auto* table = table_of(dlg);
        auto* add_btn = button_of(dlg, "Add");
        REQUIRE(add_btn != nullptr);

        add_btn->click();
        REQUIRE(table->rowCount() == 4);
        // The new row carries id 4 (max + 1), name "new", parent 0.
        bool found = false;
        for (int r = 0; r < table->rowCount(); ++r) {
            if (cell_text(table, r, 0) == "4") {
                found = true;
                REQUIRE(cell_text(table, r, 1) == "new");
                REQUIRE(cell_text(table, r, 2) == "0");
            }
        }
        REQUIRE(found);
    }

    SECTION("remove deletes the selected row") {
        ui::CategoriesDialog dlg(root);
        auto* table = table_of(dlg);
        auto* remove_btn = button_of(dlg, "Remove");
        REQUIRE(remove_btn != nullptr);
        REQUIRE_FALSE(remove_btn->isEnabled());

        table->selectRow(0);
        REQUIRE(remove_btn->isEnabled());
        remove_btn->click();
        REQUIRE(table->rowCount() == 2);
        // The removed row (id 1) is gone.
        for (int r = 0; r < table->rowCount(); ++r)
            REQUIRE(cell_text(table, r, 0) != "1");
    }

    SECTION("commit applies add/update/remove to the factory") {
        ui::CategoriesDialog dlg(root);
        auto* table = table_of(dlg);

        // Edit the name of id 2 and the parent of id 3.
        table->item(1, 1)->setText("Armor");
        table->item(2, 2)->setText("2");  // Poses -> child of Armour
        // Add a new category.
        button_of(dlg, "Add")->click();
        for (int r = 0; r < table->rowCount(); ++r) {
            if (cell_text(table, r, 0) == "4")
                table->item(r, 1)->setText("Weapons");
        }
        // Remove id 1 (Animations) — its child Poses was re-parented above.
        for (int r = 0; r < table->rowCount(); ++r) {
            if (cell_text(table, r, 0) == "1") {
                table->selectRow(r);
                button_of(dlg, "Remove")->click();
                break;
            }
        }

        REQUIRE(dlg.commit_changes());

        auto& factory = engine::CategoryFactory::instance();
        REQUIRE_FALSE(factory.categoryExists(1));
        REQUIRE(factory.categoryExists(2));
        REQUIRE(factory.categoryExists(3));
        REQUIRE(factory.categoryExists(4));
        REQUIRE(factory.categoryById(2)->name == "Armor");
        REQUIRE(factory.categoryById(3)->parent_id == 2);
        REQUIRE(factory.categoryById(4)->name == "Weapons");
        REQUIRE(factory.categoryById(4)->parent_id == 0);

        // Persisted to <instance_root>/categories.dat.
        std::ifstream in(root / "categories.dat");
        REQUIRE(in.good());
        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        REQUIRE(content.find("2|Armor|0") != std::string::npos);
        REQUIRE(content.find("3|Poses|2") != std::string::npos);
        REQUIRE(content.find("4|Weapons|0") != std::string::npos);
        REQUIRE(content.find("1|Animations|0") == std::string::npos);
    }

    SECTION("commit rejects a duplicate id") {
        ui::CategoriesDialog dlg(root);
        auto* table = table_of(dlg);
        table->item(1, 0)->setText("1");  // duplicate of id 1
        REQUIRE_FALSE(dlg.commit_changes());
        // Factory untouched.
        auto& factory = engine::CategoryFactory::instance();
        REQUIRE(factory.categoryById(2) != nullptr);
        REQUIRE(factory.categoryById(2)->name == "Armour");
    }

    SECTION("commit rejects a dangling parent") {
        ui::CategoriesDialog dlg(root);
        auto* table = table_of(dlg);
        table->item(2, 2)->setText("99");  // no such category
        REQUIRE_FALSE(dlg.commit_changes());
        REQUIRE(engine::CategoryFactory::instance().categoryById(3)->parent_id == 1);
    }

    SECTION("commit rejects a self-parent") {
        ui::CategoriesDialog dlg(root);
        auto* table = table_of(dlg);
        table->item(0, 2)->setText("1");  // Animations is its own parent
        REQUIRE_FALSE(dlg.commit_changes());
        REQUIRE(engine::CategoryFactory::instance().categoryById(1)->parent_id == 0);
    }

    SECTION("commit rejects a non-positive id") {
        ui::CategoriesDialog dlg(root);
        auto* table = table_of(dlg);
        table->item(0, 0)->setText("0");  // 0 is the implicit "None"
        REQUIRE_FALSE(dlg.commit_changes());
        REQUIRE(engine::CategoryFactory::instance().categoryExists(1));
    }

    SECTION("cancel leaves the factory untouched") {
        {
            ui::CategoriesDialog dlg(root);
            auto* table = table_of(dlg);
            table->item(0, 1)->setText("Changed");
            button_of(dlg, "Add")->click();
            // No commit_changes() call — the dialog is destroyed.
        }
        auto& factory = engine::CategoryFactory::instance();
        REQUIRE(factory.categoryById(1)->name == "Animations");
        REQUIRE_FALSE(factory.categoryExists(4));
    }
}