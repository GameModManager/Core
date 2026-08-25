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
// game_name and validates live as the user types (Workspace-y9c): illegal
// filename characters, reserved names, and empty input get a red border and
// an inline error message with OK disabled. On accept the name is sanitized
// (sanitize_directory_name()) and checked against existing instances
// (scan_instances()); failures are reported inline and keep the dialog open.
// Returns the sanitized name, or empty when cancelled.
[[nodiscard]] std::string prompt_instance_name(QWidget* parent,
                                               const QString& game_name);

}  // namespace ui
