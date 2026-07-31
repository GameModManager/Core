#include "ui/game_selection/game_selection_widget.h"

#include <QDir>
#include <QGridLayout>
#include <QLabel>
#include <QScrollArea>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QApplication>

namespace ui {

// -- Icon resolution --

// Build a colored circle with the game's first letter — used as a built-in icon
// when no theme or exe icon is available.
static QIcon make_builtin_icon(const std::string& game_id, const std::string& name) {
    // Pick a color based on game_id hash
    auto hash = std::hash<std::string>{}(game_id);
    QColor base;
    switch (hash % 8) {
        case 0: base = QColor(100, 149, 237); break; // cornflower blue
        case 1: base = QColor(220, 80, 80);   break; // red
        case 2: base = QColor(80, 180, 100);  break; // green
        case 3: base = QColor(200, 160, 60);  break; // gold
        case 4: base = QColor(160, 100, 200); break; // purple
        case 5: base = QColor(60, 180, 200);  break; // teal
        case 6: base = QColor(220, 140, 60);  break; // orange
        case 7: base = QColor(120, 120, 180); break; // slate
    }

    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    // Circle
    p.setPen(Qt::NoPen);
    p.setBrush(base);
    p.drawEllipse(2, 2, 60, 60);

    // First letter of display name
    QString letter;
    if (!name.empty()) {
        letter = QString::fromStdString(name).left(1).toUpper();
    } else {
        letter = QString::fromStdString(game_id).left(1).toUpper();
    }

    p.setPen(Qt::white);
    QFont f = p.font();
    f.setPointSize(24);
    f.setBold(true);
    p.setFont(f);
    p.drawText(QRect(0, 0, 64, 64), Qt::AlignCenter, letter);

    return QIcon(pm);
}

QIcon GameSelectionWidget::resolve_icon(const GameEntry& entry, const QString& themes_dir) {
    // 1. Theme icon: themes/<current>/icons/<game_id>.png
    if (!themes_dir.isEmpty()) {
        QDir theme_dir(themes_dir);
        QStringList name_filters;
        name_filters << (QString::fromStdString(entry.game_id) + ".png")
                     << (QString::fromStdString(entry.game_id) + ".svg");
        auto files = theme_dir.entryList(name_filters, QDir::Files);
        if (!files.isEmpty()) {
            QIcon theme_icon(theme_dir.filePath(files.first()));
            if (!theme_icon.isNull()) return theme_icon;
        }
    }

    // 2. Built-in icon (generated from game name)
    return make_builtin_icon(entry.game_id, entry.display_name);
}

// -- GameSelectionWidget --

GameSelectionWidget::GameSelectionWidget(QWidget* parent)
    : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* container = new QWidget();
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(40, 30, 40, 30);
    layout->setSpacing(16);

    // -- Title --
    title_ = new QLabel(tr("Welcome to GameModManager"));
    QFont title_font = title_->font();
    title_font.setPointSize(18);
    title_font.setBold(true);
    title_->setFont(title_font);
    title_->setAlignment(Qt::AlignCenter);
    layout->addWidget(title_);

    layout->addSpacing(10);

    auto* subtitle = new QLabel(tr("Select a game to manage"));
    subtitle->setObjectName("gameSelectionSubtitle");
    QFont sub_font = subtitle->font();
    sub_font.setPointSize(11);
    sub_font.setItalic(true);
    subtitle->setFont(sub_font);
    subtitle->setAlignment(Qt::AlignCenter);
    layout->addWidget(subtitle);

    layout->addSpacing(20);

    // -- Installed Games section --
    installed_label_ = new QLabel(tr("Installed Games"));
    {
        QFont f = installed_label_->font();
        f.setPointSize(13);
        f.setBold(true);
        installed_label_->setFont(f);
    }
    layout->addWidget(installed_label_);

    installed_grid_ = new QWidget();
    layout->addWidget(installed_grid_);

    layout->addSpacing(16);

    // -- Available Games section --
    available_label_ = new QLabel(tr("Available Games"));
    {
        QFont f = available_label_->font();
        f.setPointSize(13);
        f.setBold(true);
        available_label_->setFont(f);
    }
    layout->addWidget(available_label_);

    available_grid_ = new QWidget();
    layout->addWidget(available_grid_);

    layout->addStretch(1);

    scroll->setWidget(container);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
}

void GameSelectionWidget::set_games(const std::vector<GameEntry>& installed,
                                     const std::vector<GameEntry>& available) {
    // Resolve themes dir (from the themes submodule)
    QString themes_dir;
    {
        // Look for themes relative to the executable
        auto app_dir = QCoreApplication::applicationDirPath();
        // Try multiple locations
        QStringList candidates = {
            app_dir + "/themes",
            app_dir + "/../themes",
            app_dir + "/../share/GameModManager/themes",
        };
        for (const auto& c : candidates) {
            if (QDir(c).exists()) {
                themes_dir = c;
                break;
            }
        }
    }

    // Helper to populate a grid
    auto populate_grid = [&](QWidget* grid, const std::vector<GameEntry>& games, bool installed_section) {
        // Clear existing
        auto* old_layout = grid->layout();
        if (old_layout) {
            QLayoutItem* item;
            while ((item = old_layout->takeAt(0)) != nullptr) {
                if (item->widget()) item->widget()->deleteLater();
                delete item;
            }
            delete old_layout;
        }

        if (games.empty()) {
            auto* lbl = new QLabel(
                installed_section
                    ? "No games detected. Install a supported game on Steam."
                    : "No additional games available.",
                grid);
            lbl->setObjectName("gameSelectionNoInstall");
            lbl->setAlignment(Qt::AlignCenter);
            auto* lay = new QVBoxLayout(grid);
            lay->addWidget(lbl);
            return;
        }

        auto* grid_layout = new QGridLayout(grid);
        grid_layout->setContentsMargins(0, 0, 0, 0);
        grid_layout->setSpacing(16);
        int col = 0;
        for (const auto& entry : games) {
            auto* card = new GameCard(entry, grid);
            card->set_icon(resolve_icon(entry, themes_dir));
            connect(card, &GameCard::clicked, this, &GameSelectionWidget::game_selected);
            grid_layout->addWidget(card, col / 3, col % 3);
            col++;
        }
    };

    populate_grid(installed_grid_, installed, true);
    populate_grid(available_grid_, available, false);

    // Show/hide sections
    installed_label_->setVisible(!installed.empty());
    installed_grid_->setVisible(!installed.empty());
    available_label_->setVisible(!available.empty());
    available_grid_->setVisible(!available.empty());
}

}  // namespace ui
