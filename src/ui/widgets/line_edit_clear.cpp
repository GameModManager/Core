#include "ui/widgets/line_edit_clear.h"

namespace ui {

LineEditClear::LineEditClear(QWidget *parent) : QLineEdit(parent) {
  // On by construction: a filter box that can always be emptied is the reason
  // this type exists. Callers that want it gone say so explicitly.
  setClearButtonEnabled(true);
}

bool LineEditClear::is_clear_button_enabled() const {
  return QLineEdit::isClearButtonEnabled();
}

void LineEditClear::set_clear_button_enabled(bool enabled) {
  QLineEdit::setClearButtonEnabled(enabled);
}

}  // namespace ui