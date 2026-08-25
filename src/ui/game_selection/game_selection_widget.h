#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <QIcon>
#include <QPointer>
#include <QWidget>

#include "ui/game_selection/game_card.h"

class QLineEdit;
class QLabel;

namespace ui {

// Game selection screen (Workspace-4fu): a filter bar over an alphabetically
// sorted, scrollable list of games; the game-less "Generic Instance" entry is
// always last. Shown first-run (main.cpp) and by the in-app create flow
// (settings_controller).
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
    void on_icon_ready(const QString& game_id);
    void apply_filter(const QString& text);

    QLineEdit* filter_ = nullptr;
    QWidget* list_ = nullptr;
    QLabel* status_ = nullptr;
    std::vector<QPointer<GameCard>> cards_;
};

// Ask for a new instance name (MO2-style custom naming). Pre-fills with
// game_name and re-prompts until the filesystem-sanitized name is non-empty
// and unique among existing instances (scan_instances()). Returns the
// sanitized name, or empty when cancelled.
[[nodiscard]] std::string prompt_instance_name(QWidget* parent,
                                               const QString& game_name);

}  // namespace ui
