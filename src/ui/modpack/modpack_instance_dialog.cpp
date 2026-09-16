#include "ui/modpack/modpack_instance_dialog.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace ui {

ModpackInstanceDialog::ModpackInstanceDialog(
    const std::string& pack_game_id, const QString& pack_game_display,
    std::vector<ModpackInstanceChoice> instances,
    const std::string& active_folder_name, QWidget* parent)
    : QDialog(parent), instances_(std::move(instances)) {
    setWindowTitle(tr("Select Instance"));
    setMinimumWidth(420);

    auto* layout = new QVBoxLayout(this);

    auto* target = new QLabel(
        tr("This pack targets: %1").arg(pack_game_display), this);
    layout->addWidget(target);
    (void)pack_game_id;

    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel(tr("Instance:"), this));
    combo_ = new QComboBox(this);
    for (const auto& inst : instances_) {
        combo_->addItem(QString::fromStdString(inst.display_name),
                        QString::fromStdString(inst.folder_name));
    }
    // Pre-select the active instance when it matches, else the first match.
    int preselect = 0;
    for (int i = 0; i < combo_->count(); ++i) {
        if (combo_->itemData(i).toString().toStdString() ==
            active_folder_name) {
            preselect = i;
            break;
        }
    }
    combo_->setCurrentIndex(preselect);
    row->addWidget(combo_, 1);
    layout->addLayout(row);

    empty_label_ = new QLabel(
        tr("No instance matches this pack's game. Create a new instance."),
        this);
    empty_label_->setWordWrap(true);
    empty_label_->setVisible(instances_.empty());
    layout->addWidget(empty_label_);
    combo_->setEnabled(!instances_.empty());

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    append_button_ = new QPushButton(tr("Append to Instance"), this);
    append_button_->setEnabled(!instances_.empty());
    auto* create_button = new QPushButton(tr("Create New"), this);
    buttons->addWidget(append_button_);
    buttons->addWidget(create_button);
    layout->addLayout(buttons);

    connect(append_button_, &QPushButton::clicked, this, [this]() {
        mode_ = Mode::Append;
        accept();
    });
    connect(create_button, &QPushButton::clicked, this, [this]() {
        mode_ = Mode::CreateNew;
        accept();
    });
}

std::string ModpackInstanceDialog::selected_folder() const {
    if (mode_ == Mode::Cancelled || !combo_ || combo_->count() == 0) return {};
    return combo_->currentData().toString().toStdString();
}

}  // namespace ui
