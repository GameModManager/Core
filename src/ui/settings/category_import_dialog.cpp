#include "ui/settings/category_import_dialog.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QVBoxLayout>

namespace ui {

CategoryImportDialog::CategoryImportDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(tr("Nexus Category Import"));

  auto *layout = new QVBoxLayout(this);

  auto *heading = new QLabel(tr("How do you want to import the categories?"), this);
  layout->addWidget(heading);

  auto *columns = new QHBoxLayout();

  auto *strategy_box    = new QGroupBox(tr("Import Strategy"), this);
  auto *strategy_layout = new QVBoxLayout(strategy_box);
  strategy_group_       = new QButtonGroup(this);
  // Replace first-checked, as MO2's categoryimportdialog.ui ships it.
  replace_option_ = new QRadioButton(tr("Replace"), strategy_box);
  merge_option_   = new QRadioButton(tr("Merge"), strategy_box);
  replace_option_->setChecked(true);
  strategy_group_->addButton(replace_option_);
  strategy_group_->addButton(merge_option_);
  strategy_layout->addWidget(replace_option_);
  strategy_layout->addWidget(merge_option_);
  columns->addWidget(strategy_box);

  auto *options_box    = new QGroupBox(tr("Options"), this);
  auto *options_layout = new QVBoxLayout(options_box);
  assign_option_       = new QCheckBox(tr("Assign Nexus mappings"), options_box);
  assign_option_->setChecked(true);
  remap_option_ = new QCheckBox(tr("Remap existing mappings"), options_box);
  // A checked assign box on the default (Replace) strategy still leaves nothing
  // to remap onto, so MO2 ships this disabled
  // (references/modorganizer/src/categoryimportdialog.ui).
  remap_option_->setEnabled(false);
  options_layout->addWidget(assign_option_);
  options_layout->addWidget(remap_option_);
  columns->addWidget(options_box);

  layout->addLayout(columns);

  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);

  connect(strategy_group_, &QButtonGroup::buttonClicked, this,
          &CategoryImportDialog::on_strategy_clicked);
  connect(assign_option_, &QCheckBox::toggled, this,
          &CategoryImportDialog::on_assign_option_toggled);
}

CategoryImportDialog::~CategoryImportDialog() = default;

CategoryImportDialog::ImportStrategy CategoryImportDialog::strategy() const {
  if (merge_option_->isChecked())
    return Merge;
  if (replace_option_->isChecked())
    return Overwrite;
  return None;
}

bool CategoryImportDialog::assign() const {
  return assign_option_->isChecked();
}

bool CategoryImportDialog::remap() const {
  return remap_option_->isChecked();
}

void CategoryImportDialog::on_strategy_clicked(QAbstractButton *button) {
  // MO2 categoryimportdialog.cpp:57-65, unchanged: Replace leaves nothing to
  // remap onto, Merge re-enables the box.
  if (button == replace_option_) {
    remap_option_->setChecked(false);
    remap_option_->setDisabled(true);
  } else {
    remap_option_->setEnabled(true);
  }
}

void CategoryImportDialog::on_assign_option_toggled(bool checked) {
  // MO2 categoryimportdialog.cpp:67-75: only Merge + assign offers remapping.
  if (checked && strategy() == Merge)
    remap_option_->setEnabled(true);
  else {
    remap_option_->setChecked(false);
    remap_option_->setDisabled(true);
  }
}

}  // namespace ui