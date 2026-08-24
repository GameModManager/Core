#include "ui/widgets/game_path_banner.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>

namespace ui {

GamePathBanner::GamePathBanner(QWidget *parent) : QWidget(parent) {
  auto *layout = new QHBoxLayout(this);
  layout->setContentsMargins(8, 6, 8, 6);

  auto *icon = new QLabel(this);
  icon->setPixmap(
      style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(16, 16));
  layout->addWidget(icon);

  layout->addWidget(new QLabel(
      tr("No game directory is set for this instance: mods can be managed "
         "and downloaded, but scanning, deployment and launching need a "
         "game path."),
      this));

  auto *button = new QPushButton(tr("Set Game Path..."), this);
  button->setToolTip(tr("Choose the game's installation directory"));
  layout->addStretch(1);
  layout->addWidget(button);

  connect(button, &QPushButton::clicked, this,
          [this]() { emit pick_requested(); });

  // Hidden until an instance without a game dir is loaded.
  hide();
}

void GamePathBanner::set_instance_state(bool instance_loaded,
                                        bool has_game_dir) {
  setVisible(instance_loaded && !has_game_dir);
}

} // namespace ui
