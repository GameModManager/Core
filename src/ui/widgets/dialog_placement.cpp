#include "ui/widgets/dialog_placement.h"

#include "ui/settings/settings.h"

#include <QDialog>
#include <QGuiApplication>
#include <QScreen>
#include <QWidget>

void center_dialog_on_screen(QDialog *dialog) {
  if (dialog == nullptr || !Settings::instance().center_dialogs())
    return;

  // Prefer the screen the parent is on: that is the screen the dialog will
  // overlap, and it is the only choice that is right in a multi-monitor setup
  // where the window was last used elsewhere.
  QScreen *screen =
      dialog->parentWidget() != nullptr ? dialog->parentWidget()->screen() : nullptr;
  if (screen == nullptr)
    screen = QGuiApplication::screenAt(dialog->frameGeometry().center());
  if (screen == nullptr)
    screen = QGuiApplication::primaryScreen();
  if (screen == nullptr)
    return;

  const auto area = screen->availableGeometry();
  const auto size = dialog->size();
  dialog->move(area.center() - QPoint(size.width() / 2, size.height() / 2));
}
