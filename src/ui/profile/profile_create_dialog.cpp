#include "ui/profile/profile_create_dialog.h"

#include "engine/util/fs_utils.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>

namespace ui {

ProfileCreateDialog::ProfileCreateDialog(const QStringList &existing_profiles,
                                         const QString &copy_source, QWidget *parent)
    : QDialog(parent) {
  setWindowTitle(tr("Create Profile"));

  auto *form = new QFormLayout(this);

  name_edit_ = new QLineEdit(this);
  name_edit_->setPlaceholderText(tr("Profile name"));
  form->addRow(tr("Name:"), name_edit_);

  copy_combo_ = new QComboBox(this);
  copy_combo_->addItem(tr("(fresh)"));
  for (const auto &name : existing_profiles) {
    copy_combo_->addItem(name);
  }
  if (!copy_source.isEmpty()) {
    const int idx = copy_combo_->findText(copy_source);
    if (idx >= 0) {
      copy_combo_->setCurrentIndex(idx);
    }
  }
  form->addRow(tr("Copy from:"), copy_combo_);

  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  form->addRow(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
    // MO2 ProfileInputDialog::exec (profileinputdialog.cpp:38-40) folds the
    // typed name through fixDirectoryName before the dialog can be accepted,
    // and MO2 ProfilesDialog (profilesdialog.cpp:217-227) refuses the copy
    // outright with "Invalid profile name" when nothing usable is left.
    // sanitize_directory_name is the same fix-up; it returns empty when the
    // name cannot be a folder name at all.
    const QString fixed = QString::fromStdString(
        engine::sanitize_directory_name(profile_name().toStdString()));
    if (fixed.isEmpty()) {
      QMessageBox::warning(this, tr("Invalid name"), tr("Invalid profile name"));
      return;
    }
    name_edit_->setText(fixed);
    accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  connect(name_edit_, &QLineEdit::textChanged, this, [this]() {
    update_ok_button();
  });
  update_ok_button();
}

QString ProfileCreateDialog::profile_name() const {
  return name_edit_->text().trimmed();
}

QString ProfileCreateDialog::copy_source() const {
  const QString text = copy_combo_->currentText();
  if (text == tr("(fresh)")) {
    return {};
  }
  return text;
}

void ProfileCreateDialog::update_ok_button() {
  // The button box is a child; find the OK button to gate it on a non-empty
  // name (the engine validates the full name on create).
  for (auto *btn : findChildren<QPushButton *>()) {
    if (btn->text() == tr("OK")) {
      btn->setEnabled(!profile_name().isEmpty());
      return;
    }
  }
}

}  // namespace ui