#pragma once

#include <QDialog>

#include <QStringList>

class QDialogButtonBox;
class QLineEdit;
class QListWidget;

namespace ui {

// MO2 ListDialog port (listdialog.{cpp,ui}): a small modal picker with a
// scrollable choice list, a live case-insensitive filter box (with a native
// clear button), auto-select when the filter yields exactly one match, a red
// list border while a filter is active, double-click-to-accept, and Ok/Cancel.
// Choice items may carry parallel payload data (setChoiceData) so callers can
// round-trip real ids instead of re-resolving display names. Geometry is
// saved/restored via Settings::listdialog_window_geometry.
class ListDialog : public QDialog {
  Q_OBJECT
public:
  explicit ListDialog(QWidget *parent = nullptr);

  int exec() override; // saves and restores geometry

  void setChoices(const QStringList &choices);
  // Optional parallel payloads, one per choice (length must match choices).
  void setChoiceData(const QList<QVariant> &data);
  void setCurrentRow(int row);

  // Selected item text, or empty when nothing is selected.
  QString getChoice() const;
  // Selected item payload (invalid when nothing is selected / no data set).
  QVariant getChoiceData() const;

private slots:
  void on_filter_textChanged(const QString &filter);

private:
  void apply_filter();

  QListWidget *list_ = nullptr;
  QLineEdit *filter_ = nullptr;
  QDialogButtonBox *buttons_ = nullptr;
  QStringList choices_;
  QList<QVariant> data_;
};

} // namespace ui
