#pragma once

#include "ui/widgets/line_edit_clear.h"

#include <QWidget>

class QPushButton;
class QTableWidget;

namespace ui {

// Persistent filter bar below the right panel's tab widget.
// Survives tab switches - the same text filters whichever tab is active.
class RightFilterBar : public QWidget {
  Q_OBJECT
public:
  explicit RightFilterBar(QWidget *parent = nullptr);

  [[nodiscard]] QString filter_text() const;

  // Ctrl+F / Escape pair, MO2 setFilterShortcuts
  // (references/modorganizer/src/mainwindow.cpp:204-232) - the same contract
  // ModFilterBar offers, because MainWindow routes one window-scoped pair of
  // shortcuts across both bars.
  void focus_filter();
  // Clears the text (fires filter_changed). Unconditional, like ModFilterBar.
  void clear_filter();
  [[nodiscard]] bool filter_has_focus() const;

  // Apply the current filter text to the given table
  void apply_to(QTableWidget *table) const;

  // Show/hide the LOOT sort shortcut. Only meaningful on the Plugins tab.
  void set_sort_visible(bool visible);

signals:
  void filter_changed(const QString &text);
  void sort_requested();

private:
  LineEditClear *filter_edit_ = nullptr;
  QPushButton *sort_button_   = nullptr;
};

}  // namespace ui
