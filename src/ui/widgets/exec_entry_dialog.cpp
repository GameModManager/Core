#include "ui/widgets/exec_entry_dialog.h"
#include "ui/widgets/exec_entry_content_widget.h"

#include <QVBoxLayout>

namespace ui {

ExecEntryDialog::ExecEntryDialog(
    const std::filesystem::path &game_dir,
    const QVector<QPair<QString, QString>> &mod_list,
    const QVector<ExecEntry> &initial_entries,
    const std::filesystem::path &icon_cache_dir, QWidget *parent)
    : QDialog(parent) {
  setWindowTitle(tr("Modify Executables"));

  auto *layout = new QVBoxLayout(this);
  content_ = new ExecEntryContentWidget(game_dir, mod_list, initial_entries,
                                        icon_cache_dir, this);
  layout->addWidget(content_);

  // The content widget's Save/Cancel buttons carry the dialog's accept /
  // reject semantics: Save validates internally and only emits
  // save_requested() when the entries are valid.
  connect(content_, &ExecEntryContentWidget::save_requested, this,
          &QDialog::accept);
  connect(content_, &ExecEntryContentWidget::cancel_requested, this,
          &QDialog::reject);
}

QVector<ExecEntry> ExecEntryDialog::entries() const {
  return content_->entries();
}

} // namespace ui
