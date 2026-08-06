#include "ui/modinfo/mod_info_tab.h"

namespace ui {

ModInfoTab::ModInfoTab(QWidget* parent) : QWidget(parent) {}

ModInfoTab::~ModInfoTab() = default;

void ModInfoTab::set_has_data(bool has) {
    has_data_ = has;
}

}  // namespace ui
