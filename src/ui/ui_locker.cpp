#include "ui/ui_locker.h"

#include <QToolBar>

#include "ui/main_window/main_window.h"
#include "ui/widgets/menu_bar.h"
#include "ui/widgets/profile_bar.h"
#include "ui/widgets/status_bar.h"

namespace ui {

Locker::Locker(MainWindow *window) : w_(window) {}

void Locker::set_enabled(bool enabled) {
  // Lock or unlock the whole manager surface (mod list, panels, console,
  // menus, toolbars). The install dialogs (FOMOD wizard, name confirm,
  // overwrite query, progress popup) are top-level children of the main
  // window, NOT of the disabled content widgets, so they stay interactive
  // while the manager itself is greyed out - the same shape MO2's
  // UILocker produces.
  if (w_->centralWidget())
    w_->centralWidget()->setEnabled(enabled);
  if (w_->menu_bar_)
    w_->menu_bar_->setEnabled(enabled);
  if (w_->toolbar_area_)
    w_->toolbar_area_->setEnabled(enabled);
  if (w_->profile_bar_)
    w_->profile_bar_->setEnabled(enabled);
  if (w_->status_bar_)
    w_->status_bar_->setEnabled(enabled);
}

} // namespace ui