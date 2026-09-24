#include "ui/modpack/modpack_instance_dialog.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace ui {

ModpackInstanceDialog::ModpackInstanceDialog(
    const std::string &pack_game_id, const QString &pack_game_display,
    std::vector<ModpackInstanceChoice> instances, const std::string &active_folder_name,
    QWidget *parent)
    : QDialog(parent), instances_(std::move(instances)) {
  setWindowTitle(tr("Select Instance"));
  setMinimumSize(440, 180);
  setSizeGripEnabled(true);

  auto *layout = new QVBoxLayout(this);

  const QString shown_game = pack_game_display.isEmpty()
                                 ? QString::fromStdString(pack_game_id)
                                 : pack_game_display;
  auto *target      = new QLabel(tr("This pack targets: %1").arg(shown_game), this);
  QFont target_font = target->font();
  target_font.setBold(true);
  target->setFont(target_font);
  target->setWordWrap(true);
  layout->addWidget(target);
  (void)pack_game_id;

  auto *row = new QHBoxLayout();
  row->addWidget(new QLabel(tr("Instance:"), this));
  combo_ = new QComboBox(this);
  for (const auto &inst : instances_) {
    const QString display = QString::fromStdString(inst.display_name);
    const QString folder  = QString::fromStdString(inst.folder_name);
    // Show the folder id next to the display name when they differ so
    // two same-named instances stay distinguishable.
    const QString label = (!display.isEmpty() && display != folder)
                              ? tr("%1 (%2)").arg(display, folder)
                              : (display.isEmpty() ? folder : display);
    combo_->addItem(label, folder);
    combo_->setItemData(combo_->count() - 1, folder, Qt::ToolTipRole);
  }
  // Pre-select the active instance when it matches, else the first match.
  int preselect = 0;
  for (int i = 0; i < combo_->count(); ++i) {
    if (combo_->itemData(i).toString().toStdString() == active_folder_name) {
      preselect = i;
      break;
    }
  }
  combo_->setCurrentIndex(preselect);
  row->addWidget(combo_, 1);
  layout->addLayout(row);

  empty_label_ = new QLabel(
      tr("No instance matches this pack's game. Create a new instance."), this);
  empty_label_->setWordWrap(true);
  empty_label_->setVisible(instances_.empty());
  layout->addWidget(empty_label_);
  combo_->setEnabled(!instances_.empty());

  auto *buttons = new QHBoxLayout();
  buttons->addStretch(1);
  append_button_ = new QPushButton(tr("Append to Instance"), this);
  append_button_->setEnabled(!instances_.empty());
  append_button_->setDefault(!instances_.empty());
  auto *create_button = new QPushButton(tr("Create New"), this);
  auto *cancel_button = new QPushButton(tr("Cancel"), this);
  buttons->addWidget(append_button_);
  buttons->addWidget(create_button);
  buttons->addWidget(cancel_button);
  layout->addLayout(buttons);

  connect(append_button_, &QPushButton::clicked, this, [this]() {
    mode_ = Mode::Append;
    accept();
  });
  connect(create_button, &QPushButton::clicked, this, [this]() {
    mode_ = Mode::CreateNew;
    accept();
  });
  connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);
}

std::string ModpackInstanceDialog::selected_folder() const {
  if (mode_ == Mode::Cancelled || !combo_ || combo_->count() == 0)
    return {};
  return combo_->currentData().toString().toStdString();
}

}  // namespace ui
