#pragma once

#include <QDialog>

class QAbstractButton;
class QButtonGroup;
class QCheckBox;
class QRadioButton;

namespace ui {

// MO2's CategoryImportDialog (references/modorganizer/src/categoryimportdialog.h,
// .cpp and categoryimportdialog.ui), asked before importing a fetched Nexus
// category list. Three questions, all of them real behaviour rather than
// decoration:
//
//   strategy - Merge keeps the categories already on screen and adds only what
//              is missing; Replace (MO2's "Overwrite") drops them first.
//              Defaults to Replace, as the .ui does.
//   assign   - record the Nexus mapping on every row the import adds, so the
//              installed mod lands in the right local category later
//              (resolve_nexus_id). Checked by default, as the .ui does.
//   remap    - for a category already in the list under the same name, move the
//              Nexus mapping onto that row; and drop the mapping where there is
//              no such row. Only offered while the strategy is Merge: with
//              Replace the whole table was just emptied, so there is nothing to
//              remap onto (categoryimportdialog.cpp:57-75).
//
// Both rules are one line each in MO2 and one line each here. Neither is
// guesswork about what "should" happen - MO2 disables the remap box in exactly
// those two cases and this dialog does the same.
class CategoryImportDialog : public QDialog {
  Q_OBJECT

public:
  // MO2's own enum (categoryimportdialog.h:19-25). None is a real answer: it is
  // what strategy() returns when neither radio is checked.
  enum ImportStrategy {
    None = 0,
    Overwrite,
    Merge,
  };

  explicit CategoryImportDialog(QWidget *parent = nullptr);
  ~CategoryImportDialog() override;

  [[nodiscard]] ImportStrategy strategy() const;
  [[nodiscard]] bool assign() const;
  [[nodiscard]] bool remap() const;

private:
  void on_strategy_clicked(QAbstractButton *button);
  void on_assign_option_toggled(bool checked);

  QButtonGroup *strategy_group_ = nullptr;
  QRadioButton *merge_option_   = nullptr;
  QRadioButton *replace_option_ = nullptr;
  QCheckBox *assign_option_     = nullptr;
  QCheckBox *remap_option_      = nullptr;
};

}  // namespace ui