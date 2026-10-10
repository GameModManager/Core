#include "ui/modinfo/mod_info_tab.h"

namespace ui {

ModInfoTab::ModInfoTab(QWidget *parent) : QWidget(parent) {}

ModInfoTab::~ModInfoTab() = default;

void ModInfoTab::set_has_data(bool has) {
  if (has_data_ == has)
    return;
  has_data_ = has;
  // MO2 ModInfoDialogTab::setHasData (modinfodialogtab.cpp:137) emits so the
  // dialog can re-colour the tab bar; without it a tab greyed on open stays
  // grey after the user gives it content.
  emit has_data_changed();
}

}  // namespace ui
