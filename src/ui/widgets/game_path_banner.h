#pragma once

#include <QWidget>

namespace ui {

// Prominent "Set Game Path" banner shown at the top of the main window while
// a game-less instance is loaded (Workspace-tnj): downloads and instance-side
// management work, but scanning, deployment and launching need a game
// directory. Native widgets only — prominence comes from placement and copy,
// not hardcoded colors (QPalette-first convention).
class GamePathBanner : public QWidget {
  Q_OBJECT
public:
  explicit GamePathBanner(QWidget *parent = nullptr);

  // Visible iff an instance is loaded AND it has no game dir.
  void set_instance_state(bool instance_loaded, bool has_game_dir);

signals:
  // The user picked a game directory in the banner's picker.
  void game_path_picked(const QString &dir);
};

} // namespace ui
