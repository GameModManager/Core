#pragma once

#include <QLineEdit>

namespace ui {

// A QLineEdit with the inline "clear" affordance already switched on: a
// trailing action button that empties the field, shown only while there is
// something to clear.
//
// MO2 parity: MOBase::LineEditClear (uibase/lineeditclear.h). MO2 hand-paints
// a borderless QToolButton inside the frame because it predates the Qt native
// action; this uses Qt's own button (QLineEdit::setClearButtonEnabled), which
// draws the same affordance and picks up the platform style. The visible
// behaviour is identical, so there is nothing to gain from reimplementing it.
// Qt has no API for where the clear button sits - it is always trailing - so
// that is not a knob here either.
//
// The class exists as a named type rather than a repeated
// setClearButtonEnabled(true) call so every filter box in the app has one
// concept behind it, and so a .ui file has something to promote.
//
// Drop-in replacement for QLineEdit: the full QLineEdit API works unchanged,
// including set_clear_button_enabled(false) to switch the affordance off again.
//
// No Q_PROPERTY here on purpose: QLineEdit has exposed `clearButtonEnabled`
// itself since Qt 6.4, so re-declaring it would put two properties with one
// name into one metaobject chain. QSS keeps working through the inherited
// property; the two methods below are the house-style snake_case layer over it.
class LineEditClear : public QLineEdit {
  Q_OBJECT

public:
  explicit LineEditClear(QWidget *parent = nullptr);

  [[nodiscard]] bool is_clear_button_enabled() const;
  void set_clear_button_enabled(bool enabled);
};

}  // namespace ui