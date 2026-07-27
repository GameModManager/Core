#pragma once

#include <filesystem>
#include <string>

#include <QFrame>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace ui {

// A single game entry for the selection screen.
struct GameEntry {
    std::string game_id;
    std::string display_name;
    uint32_t steam_appid = 0;
    bool installed = false;          // auto-detected via Steam VDF
    std::filesystem::path install_path;
};

// A clickable card showing a game's icon and name.
// Two visual states: installed (solid highlight) and available (muted).
class GameCard : public QFrame {
    Q_OBJECT

public:
    explicit GameCard(const GameEntry& entry, QWidget* parent = nullptr);

    [[nodiscard]] const GameEntry& entry() const { return entry_; }
    void set_icon(const QIcon& icon);

signals:
    void clicked(const GameEntry& entry);

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    GameEntry entry_;
    QLabel* icon_label_ = nullptr;
    QLabel* name_label_ = nullptr;
};

}  // namespace ui
