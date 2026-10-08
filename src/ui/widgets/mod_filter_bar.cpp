#include "ui/widgets/mod_filter_bar.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QToolButton>

namespace ui {

ModFilterBar::ModFilterBar(QWidget *parent) : QWidget(parent) {
  auto *layout = new QHBoxLayout(this);
  layout->setContentsMargins(4, 2, 4, 2);
  layout->setSpacing(4);

  setWhatsThis(tr("Filter the mod list. The box narrows it by text, the dropdown "
                  "narrows it to one group - enabled, disabled, mods with conflicts, "
                  "and so on - and the button at the far left shows or hides the "
                  "category filter."));

  // Category filter panel toggle [<< / >>] (MO2 parity): shows/hides the
  // checkable category tree. The text flips to indicate the panel state.
  category_toggle_btn_ = new QToolButton(this);
  category_toggle_btn_->setText(">>");
  category_toggle_btn_->setToolTip(tr("Show / hide the category filter panel"));
  category_toggle_btn_->setFixedWidth(30);
  category_toggle_btn_->setCheckable(true);
  layout->addWidget(category_toggle_btn_);

  connect(category_toggle_btn_, &QToolButton::toggled, this, [this](bool on) {
    category_toggle_btn_->setText(on ? "<<" : ">>");
    emit category_panel_toggled(on);
  });

  // Filter text input. The placeholder is MO2's plain "Filter"
  // (references/modorganizer/src/mainwindow.ui:584) - not "Filter...".
  filter_edit_ = new LineEditClear(this);
  filter_edit_->setPlaceholderText(tr("Filter"));
  layout->addWidget(filter_edit_, 1);

  connect(filter_edit_, &QLineEdit::textChanged, this, &ModFilterBar::filter_changed);

  // Groups dropdown
  group_combo_ = new QComboBox(this);
  group_combo_->addItem(tr("All"));
  group_combo_->addItem(tr("Enabled"));
  group_combo_->addItem(tr("Disabled"));
  group_combo_->addItem(tr("Conflicts"));
  group_combo_->addItem(tr("FOMOD"));
  group_combo_->addItem(tr("Separators"));
  group_combo_->setMinimumWidth(100);
  layout->addWidget(group_combo_);

  connect(group_combo_, &QComboBox::currentTextChanged, this,
          &ModFilterBar::group_changed);

  // The wheel must not change the group either (MO2 parity, mainwindow.cpp:378-380).
  // A combo this close to the mod list gets a wheel event by accident while the
  // user is scrolling the list, and an accidental switch to "Disabled" or
  // "Conflicts" looks like the mod list lost rows. This combo, like every other
  // one, is covered by the single application-wide guard installed in
  // install_combo_wheel_guard().
}

QString ModFilterBar::filter_text() const {
  return filter_edit_->text();
}

void ModFilterBar::focus_filter() {
  filter_edit_->setFocus();
  filter_edit_->selectAll();
}

void ModFilterBar::clear_filter() {
  filter_edit_->clear();
}

bool ModFilterBar::filter_has_focus() const {
  return filter_edit_->hasFocus();
}

QString ModFilterBar::current_group() const {
  return group_combo_->currentText();
}

}  // namespace ui
