#pragma once

#include "ui/modinfo/mod_info_tab.h"

#include <memory>

class QComboBox;
class QTreeWidget;
class QTreeWidgetItem;

namespace engine {
class Categories;
}

namespace ui {

// MO2's Categories tab: a checkable category tree (from the instance's
// categories.dat) plus a "primary category" combo listing the checked ones.
// Checking an item auto-checks its ancestors; every change is persisted to the
// mod's meta as MO2's "category = <primary>,<rest...>" CSV (internal ids).
class CategoriesTab : public ModInfoTab {
    Q_OBJECT
public:
    explicit CategoriesTab(QWidget* parent = nullptr);
    ~CategoriesTab() override;

    void set_mod(const ModInfoData& data) override;
    void save_state() override;

private:
    void rebuild();
    void add_children(QTreeWidgetItem* root, int parent_id);
    void update_primary();
    void add_checked(QTreeWidgetItem* node);
    void save_tree();
    void persist();
    void on_item_changed(QTreeWidgetItem* item, int column);

    QTreeWidget* tree_ = nullptr;
    QComboBox* primary_ = nullptr;
    std::shared_ptr<engine::Categories> categories_;
    int primary_id_ = 0;
    bool rebuilding_ = false;
};

}  // namespace ui
