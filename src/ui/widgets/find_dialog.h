#pragma once

#include <QDialog>

class QCheckBox;
class QEvent;
class QLabel;
class QLineEdit;
class QPushButton;

namespace ui {

// Find-in-text for a plain text editor (MO2's MOBase::FindDialog,
// uibase/finddialog.h). Find only - there is no replace, because MO2's has
// none either. The dialog owns no text: it reports a pattern and a request to
// advance, and the editor that opened it does the searching.
//
// Two signals, matching MO2's: patternChanged fires as the user types (so the
// editor can scroll to the first hit immediately) and findNext fires on Find
// Next (and on Return in the pattern box). The editor reads the current
// pattern with pattern() when it handles findNext, so a signal without the
// value is not a gap.
class FindDialog : public QDialog {
  Q_OBJECT
public:
  explicit FindDialog(QWidget *parent = nullptr);

  [[nodiscard]] QString pattern() const;
  // True when the match is case-sensitive. Off by default, like MO2.
  [[nodiscard]] bool case_sensitive() const;

signals:
  void patternChanged(const QString &pattern);
  void findNext();

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  void find_next();

  QLineEdit *pattern_edit_ = nullptr;
  QCheckBox *case_box_     = nullptr;
  QLabel *status_          = nullptr;
  QPushButton *next_btn_   = nullptr;
};

}  // namespace ui