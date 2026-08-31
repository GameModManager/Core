#pragma once

#include "ui/modinfo/mod_info_tab.h"

class QFileSystemModel;
class QModelIndex;
class QPoint;
class QTreeView;

namespace ui {

// MO2's Filetree tab: the mod's whole folder tree with a right-click menu for
// Open / Preview / Explore / Rename / Delete / Hide / New Folder. All actions
// work on the real filesystem (no VFS - mirrors the Data tab's "no VFS"
// stance). The tree auto-updates via QFileSystemModel's watcher.
class FiletreeTab : public ModInfoTab {
    Q_OBJECT
public:
    explicit FiletreeTab(QWidget* parent = nullptr);
    ~FiletreeTab() override;

    void set_mod(const ModInfoData& data) override;

private:
    void show_menu(const QPoint& pos);
    QString selected_path() const;
    void on_open();
    void on_preview();
    void on_explore();
    void on_rename();
    void on_delete();
    void on_hide();
    void on_new_folder();

    QTreeView* tree_ = nullptr;
    QFileSystemModel* model_ = nullptr;
    QString root_path_;
};

}  // namespace ui
