#pragma once

#include "ui/widgets/line_edit_clear.h"

#include <QWidget>

class QComboBox;
class QToolButton;

namespace ui {

class ModFilterBar : public QWidget {
  Q_OBJECT
public:
  explicit ModFilterBar(QWidget *parent = nullptr);

  [[nodiscard]] QString filter_text() const;
  [[nodiscard]] QString current_group() const;

  // Ctrl+F / Escape pair, MO2 setFilterShortcuts
  // (references/modorganizer/src/mainwindow.cpp:204-232). The window owns
  // the shortcuts; the bar owns what they do.
  void focus_filter();
  // Clears the text (fires filter_changed). Unconditional - clearing an
  // already-empty filter is a no-op here, but the caller still hands focus
  // back, exactly as MO2's reset lambda does (mainwindow.cpp:211-214).
  void clear_filter();
  [[nodiscard]] bool filter_has_focus() const;

signals:
  void filter_changed(const QString &text);
  void group_changed(const QString &group);
  // Emitted when the << / >> category-panel toggle is clicked; `visible` is
  // the new panel state (true = panel shown).
  void category_panel_toggled(bool visible);

private:
  QToolButton *category_toggle_btn_ = nullptr;
  QComboBox *group_combo_           = nullptr;
  LineEditClear *filter_edit_       = nullptr;
};

}  // namespace ui
