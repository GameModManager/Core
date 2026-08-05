#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <QIcon>
#include <QWidget>

#include "ui/game_selection/game_card.h"

class QLabel;

namespace ui {

// First-run screen shown when no instances exist.
// Displays "Installed Games" (auto-detected) and "Available Games" (known but not detected).
class GameSelectionWidget : public QWidget {
    Q_OBJECT

public:
    explicit GameSelectionWidget(QWidget* parent = nullptr);

    // Populate with detected + available games.
    void set_games(const std::vector<GameEntry>& installed,
                   const std::vector<GameEntry>& available);

signals:
    void game_selected(const GameEntry& entry);

private:
    static QIcon resolve_icon(const GameEntry& entry);

    QLabel* title_ = nullptr;
    QLabel* installed_label_ = nullptr;
    QWidget* installed_grid_ = nullptr;
    QLabel* available_label_ = nullptr;
    QWidget* available_grid_ = nullptr;
};

}  // namespace ui
