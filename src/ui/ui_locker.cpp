#include "ui/ui_locker.h"

#include <QToolBar>

#include "ui/main_window/main_window.h"
#include "ui/settings/settings.h"
#include "ui/widgets/menu_bar.h"
#include "ui/widgets/profile_bar.h"
#include "ui/widgets/status_bar.h"

namespace ui {

Locker::Locker(MainWindow *window) : w_(window) {}

bool Locker::lock(LockReason reason) {
  // MO2's lock_gui gate, processrunner.cpp:860-868. OutputRequired overrides
  // the setting: the operation's output has to be read back and two concurrent
  // ones are impossible, so the user is told what is happening whether they
  // asked for locking or not.
  if (reason != LockReason::OutputRequired && !Settings::instance().lock_gui())
    return false;

  // Lock or lock-nothing: the whole manager surface (mod list, panels,
  // console, menus, toolbars). Only these five members are touched, so
  // everything else MO2's disableAll() walks over is skipped by construction -
  // message boxes, the lock overlay, and child dialogs are not named here and
  // stay interactive. That is the same shape MO2's UILocker produces, and the
  // install dialogs (FOMOD wizard, name confirm, overwrite query, progress
  // popup) are top-level children of the main window, NOT of the disabled
  // content widgets, so they stay usable while the manager is greyed out.
  disable(w_->centralWidget());
  disable(w_->menu_bar_);
  disable(w_->toolbar_area_);
  disable(w_->profile_bar_);
  disable(w_->status_bar_);
  return true;
}

void Locker::unlock() {
  for (auto &w : disabled_) {
    if (w)
      w->setEnabled(true);
  }
  disabled_.clear();
}

void Locker::disable(QWidget *w) {
  if (w && w->isEnabled()) {
    w->setEnabled(false);
    disabled_.push_back(w);
  }
}

}  // namespace ui