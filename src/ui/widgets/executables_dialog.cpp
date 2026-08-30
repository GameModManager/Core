#include "ui/widgets/executables_dialog.h"
#include "ui/widgets/executables_entry.h"

#include <QVBoxLayout>

namespace ui {
namespace Executables {

Dialog::Dialog(
    const std::filesystem::path &game_dir,
    const QVector<QPair<QString, QString>> &mod_list,
    const QVector<Entry> &initial_entries,
    const std::filesystem::path &icon_cache_dir, QWidget *parent)
    : QDialog(parent) {
  setWindowTitle(tr("Modify Executables"));

  auto *layout = new QVBoxLayout(this);
  content_ = new ContentWidget(game_dir, mod_list, initial_entries,
                                        icon_cache_dir, this);
  layout->addWidget(content_);

  // The content widget's Save/Cancel buttons carry the dialog's accept /
  // reject semantics: Save validates internally and only emits
  // save_requested() when the entries are valid.
  connect(content_, &ContentWidget::save_requested, this,
          &QDialog::accept);
  connect(content_, &ContentWidget::cancel_requested, this,
          &QDialog::reject);
}

QVector<Entry> Dialog::entries() const {
  return content_->entries();
}

} // namespace Executables
} // namespace ui
