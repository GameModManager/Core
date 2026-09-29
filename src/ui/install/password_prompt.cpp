#include "ui/install/password_prompt.h"

#include <QApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QMetaObject>
#include <QThread>

namespace ui {

namespace {

  bool ask_password_impl(const QString &archive_name, QString &password,
                         QWidget *parent) {
    bool ok   = false;
    // MO2's two strings: the title is "Password required" and the field is
    // "Password". Naming the archive in the label is the one addition, so a
    // blocked download can be identified from the dialog itself.
    const QString label = archive_name.isEmpty()
                              ? QObject::tr("Password")
                              : QObject::tr("Password for %1").arg(archive_name);
    const QString entered = QInputDialog::getText(
        parent, QObject::tr("Password required"), label, QLineEdit::Password, QString(),
        &ok);
    if (!ok || entered.isEmpty())
      return false;
    password = entered;
    return true;
  }

}  // namespace

bool ask_password(const QString &archive_name, QString &password, QWidget *parent) {
  if (QThread::currentThread() == qApp->thread()) {
    return ask_password_impl(archive_name, password, parent);
  }
  // Marshal onto the main thread and block until the modal dialog is done. The
  // extract stage runs on the pipeline thread, so this is the normal path.
  bool accepted = false;
  QMetaObject::invokeMethod(
      qApp,
      [&] {
        accepted = ask_password_impl(archive_name, password, parent);
      },
      Qt::BlockingQueuedConnection);
  return accepted;
}

}  // namespace ui
