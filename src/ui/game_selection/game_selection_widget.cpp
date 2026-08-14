#include "ui/game_selection/game_selection_widget.h"

#include <QGridLayout>
#include <QLabel>
#include <QScrollArea>

#include "ui/theme/icon_manager.h"
#include "ui/widgets/game_icon_cache.h"

namespace ui {

// -- Icon resolution --

QIcon GameSelectionWidget::resolve_icon(const GameEntry& entry) {
    // Game icons are a logical key too: a theme can ship themes/<theme>/icons/
    // <game_id>.png, and IconManager falls back to system/base pack before the
    // generated letter avatar here.
    QIcon icon = engine::IconManager::instance().resolve_icon(
        QString::fromStdString(entry.game_id));
    if (!icon.isNull()) return icon;
    // Declared icon from the global cache (async fetch if missing), else the
    // letter avatar.
    return GameIconCache::instance().icon_for(
        QString::fromStdString(entry.game_id),
        QString::fromStdString(entry.display_name), 64);
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

    // Swap an async-downloaded icon into its matching card(s) when it lands
    connect(&GameIconCache::instance(), &GameIconCache::icon_ready,
            this, &GameSelectionWidget::on_icon_ready);
}

void GameSelectionWidget::on_icon_ready(const QString& game_id) {
    std::string gid = game_id.toStdString();
    for (auto& card : cards_) {
        if (card && card->entry().game_id == gid) {
            card->set_icon(resolve_icon(card->entry()));
        }
    }
}

void GameSelectionWidget::set_games(const std::vector<GameEntry>& installed,
                                     const std::vector<GameEntry>& available) {
    // Cards are rebuilt below; drop the old QPointers.
    cards_.clear();

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
            card->set_icon(resolve_icon(entry));
            cards_.push_back(card);
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
