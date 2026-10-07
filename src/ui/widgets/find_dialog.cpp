#include "ui/widgets/find_dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace ui {

FindDialog::FindDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(tr("Find"));
  setWhatsThis(tr("Search the file for text. Typing jumps to the first match; "
                  "Find Next steps to the following one."));

  auto *layout  = new QVBoxLayout(this);
  auto *form    = new QFormLayout;
  pattern_edit_ = new QLineEdit(this);
  pattern_edit_->setPlaceholderText(tr("Find what"));
  pattern_edit_->setClearButtonEnabled(true);
  form->addRow(tr("Find:"), pattern_edit_);

  case_box_ = new QCheckBox(tr("Match case"), this);
  form->addRow(QString(), case_box_);

  // "No results" / "N of M" reads here too, so the editor can report a failed
  // search without the dialog guessing a count it cannot know.
  status_ = new QLabel(this);
  status_->setWordWrap(true);
  form->addRow(QString(), status_);

  layout->addLayout(form);

  // Find Next + Close, matching MO2's two-button dialog.
  next_btn_ = new QPushButton(tr("Find Next"), this);
  next_btn_->setDefault(true);
  next_btn_->setEnabled(false);
  auto *buttons = new QDialogButtonBox(this);
  buttons->addButton(next_btn_, QDialogButtonBox::ActionRole);
  auto *close = buttons->addButton(QDialogButtonBox::Close);
  layout->addWidget(buttons);

  connect(close, &QPushButton::clicked, this, &QDialog::close);
  connect(pattern_edit_, &QLineEdit::textChanged, this, [this](const QString &text) {
    next_btn_->setEnabled(!text.isEmpty());
    emit patternChanged(text);
  });
  connect(case_box_, &QCheckBox::toggled, this, [this](bool) {
    emit patternChanged(pattern_edit_->text());
  });
  connect(next_btn_, &QPushButton::clicked, this, &FindDialog::find_next);
  connect(pattern_edit_, &QLineEdit::returnPressed, this, &FindDialog::find_next);

  // Escape closes the dialog instead of being swallowed by the pattern box.
  // The dialog is the only thing on screen while it is open, so this cannot
  // shadow a shortcut outside it.
  installEventFilter(this);
}

bool FindDialog::eventFilter(QObject *watched, QEvent *event) {
  if (watched == this && event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->key() == Qt::Key_Escape) {
      close();
      return true;
    }
  }
  return QDialog::eventFilter(watched, event);
}

QString FindDialog::pattern() const {
  return pattern_edit_->text();
}

bool FindDialog::case_sensitive() const {
  return case_box_->isChecked();
}

void FindDialog::find_next() {
  if (pattern_edit_->text().isEmpty())
    return;
  emit findNext();
}

}  // namespace ui
