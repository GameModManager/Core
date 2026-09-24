#include "ui/widgets/error_popup.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMessageBox>

#include "ui/widgets/task_dialog.h"

namespace ui {

namespace {

  ErrorPresenter &presenter_slot() {
    static ErrorPresenter presenter;
    return presenter;
  }

  void present_by_default(TaskDialog &dialog) {
    dialog.exec();
  }

  QString error_title() {
    return QCoreApplication::translate("ErrorPopup", "Error");
  }

}  // namespace

void report_error(const QString &message, QWidget *parent, const QString &details) {
  qCritical().noquote() << "GMM error:" << message;
  TaskDialog dialog(parent, error_title());
  dialog.main(message).details(details).icon(QMessageBox::Critical);
  if (auto &presenter = presenter_slot())
    presenter(dialog);
  else
    present_by_default(dialog);
}

void critical_on_top(const QString &message, QWidget *parent) {
  qCritical().noquote() << "GMM error (on top):" << message;
  TaskDialog dialog(parent, error_title());
  dialog.main(message).icon(QMessageBox::Critical);
  dialog.setWindowFlags(dialog.windowFlags() | Qt::WindowStaysOnTopHint);
  if (auto &presenter = presenter_slot())
    presenter(dialog);
  else
    present_by_default(dialog);
}

void set_error_presenter_for_tests(ErrorPresenter presenter) {
  presenter_slot() = std::move(presenter);
}

}  // namespace ui
