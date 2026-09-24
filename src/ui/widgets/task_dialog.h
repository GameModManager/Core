#pragma once

#include <QList>
#include <QMessageBox>
#include <QString>
#include <QWidget>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDialogButtonBox;
class QEventLoop;
class QKeyEvent;
class QLabel;
class QPlainTextEdit;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace ui {

// A single choice row of the dialog: the visible title, an optional subtitle
// (rendered as the command-link description), and the StandardButton id that
// exec() returns when the row is picked. Mirrors MO2's TaskDialogButton
// (uibase report.h) 1:1 so ported call sites keep their reply logic.
struct TaskDialogButton {
  QString text;
  QString description;
  QMessageBox::StandardButton id = QMessageBox::NoButton;
};

// MO2 TaskDialog port (uibase report.{h,cpp} + taskdialog.ui): a reusable
// multi-button decision dialog with a 3-tier text hierarchy (main instruction
// / content / collapsible details), command-link choice rows, and an opt-in
// remembered answer (Settings::dialog_choice) that short-circuits exec().
//
// Fluent builder: every setter returns the dialog itself so a confirmation
// reads as a single call chain ending in one StandardButton:
//
//   ui::TaskDialog dlg(parent, tr("Remove Mods"));
//   dlg.main(...).details(...).icon(QMessageBox::Question)
//      .add_button({"Yes", "", QMessageBox::Yes})
//      .add_button({"No", "", QMessageBox::No});
//   if (dlg.exec() != QMessageBox::Yes)
//     return;
//
// Button model (MO2-exact): when at least one button was added, every button
// renders as a QCommandLinkButton in insertion order; with no buttons at all
// the dialog falls back to a single plain Ok standard button. Rejecting the
// dialog (X / Escape) always returns QMessageBox::Cancel and never persists.
//
// NOTE: this subclasses QWidget (Qt::Dialog flags + application modality +
// own event loop), not QDialog. exec() must return QMessageBox::StandardButton
// per the dialog-test contract, and C++ forbids a derived exec() that changes
// QDialog::exec()'s int return type, so the modal accept/reject loop is
// reimplemented here in miniature.
class TaskDialog : public QWidget {
public:
  explicit TaskDialog(QWidget *parent = nullptr, const QString &title = QString());

  TaskDialog &title(const QString &title);
  TaskDialog &main(const QString &text);
  TaskDialog &content(const QString &text);
  TaskDialog &details(const QString &text);
  TaskDialog &icon(QMessageBox::Icon icon);
  TaskDialog &add_button(const TaskDialogButton &button);
  // Opt-in answer persistence: a stored choice for (action, file) makes
  // exec() return it without showing the dialog. Empty action = no row.
  TaskDialog &remember(const QString &action, const QString &file = QString());
  // Injects an arbitrary widget into the content panel (reparented).
  TaskDialog &add_content(QWidget *widget);
  TaskDialog &set_minimum_width(int width);

  // Runs the dialog modally: returns the picked button's StandardButton id,
  // or QMessageBox::Cancel when the dialog is rejected. A remembered answer
  // short-circuits (the dialog is never shown); an accepted answer is
  // persisted when remember() was set.
  QMessageBox::StandardButton exec();

  // Dismisses the dialog as rejected (X / Escape path): exec() returns
  // QMessageBox::Cancel and nothing is persisted.
  void reject();

protected:
  void closeEvent(QCloseEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;

private:
  void build_buttons();
  void accept_with(QMessageBox::StandardButton id);
  bool remember_enabled() const;

  QLabel *icon_label_           = nullptr;
  QLabel *main_label_           = nullptr;
  QLabel *content_label_        = nullptr;
  QVBoxLayout *button_layout_   = nullptr;
  QVBoxLayout *content_layout_  = nullptr;
  QDialogButtonBox *button_box_ = nullptr;
  QToolButton *details_toggle_  = nullptr;
  QPlainTextEdit *details_edit_ = nullptr;
  QCheckBox *remember_check_    = nullptr;
  QComboBox *remember_combo_    = nullptr;

  QList<TaskDialogButton> buttons_;
  QString remember_action_;
  QString remember_file_;
  QMessageBox::StandardButton result_ = QMessageBox::Ok;
  bool accepted_                      = false;
  bool buttons_built_                 = false;
  QEventLoop *loop_                   = nullptr;
};

}  // namespace ui
