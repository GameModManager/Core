#pragma once

#include <QString>
#include <functional>

class QMenu;

namespace ui {

class MainWindow;
class ModActions;

// Forward declaration of SourceVisitInfo (defined in mod_list_controller.h).
struct SourceVisitInfo;

// Mod::ContextMenu — extracted context menu builder for the mod list.
//
// Builds the right-click context menu for mod list rows and adds category
// submenus. Depends on ModActions for action methods and on ModListController
// for remaining cross-controller calls (on_data_mod_info, source_visit_info).
class ModContextMenu {
public:
  ModContextMenu(MainWindow *w, ModActions *actions);

  // Set a callback for on_data_mod_info (stays in ModListController).
  void set_on_data_mod_info(std::function<void(const QString &, int)> cb);

  // Set a callback for source_visit_info (stays in ModListController).
  void set_source_visit_info(
      std::function<SourceVisitInfo(const QString &, const QString &,
                                    const QString &)>
          cb);

  // Build and connect the context menu for the mod list view.
  void setup_mod_list_context_menu();

  // Add category submenus (Change Categories + Primary Category) to a menu.
  void add_category_menus(QMenu &menu, const QString &mod_id);

private:
  MainWindow *w_ = nullptr;
  ModActions *actions_ = nullptr;

  // Callbacks for cross-controller operations.
  std::function<void(const QString &, int)> on_data_mod_info_cb_;
  std::function<SourceVisitInfo(const QString &, const QString &,
                                const QString &)>
      source_visit_info_cb_;
};

} // namespace ui
