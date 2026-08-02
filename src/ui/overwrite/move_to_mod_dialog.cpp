#include "ui/overwrite/move_to_mod_dialog.h"

#include <QDialogButtonBox>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace ui {

MoveToModDialog::MoveToModDialog(
    const std::vector<std::pair<std::string, std::string>>& mods,
    QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Move content to Mod..."));
    setMinimumSize(360, 320);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(8);

    list_ = new QListWidget(this);
    for (const auto& [folder, name] : mods) {
        auto* item = new QListWidgetItem(
            QString::fromStdString(name.empty() ? folder : name), list_);
        item->setData(Qt::UserRole, QString::fromStdString(folder));
    }
    if (list_->count() > 0) list_->setCurrentRow(0);
    layout->addWidget(list_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &MoveToModDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    if (list_->count() == 0) buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
}

std::string MoveToModDialog::selected_folder() const {
    auto* item = list_->currentItem();
    if (!item) return {};
    return item->data(Qt::UserRole).toString().toStdString();
}

}  // namespace ui
