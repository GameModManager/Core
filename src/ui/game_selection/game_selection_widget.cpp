#include "ui/game_selection/game_selection_widget.h"

#include <algorithm>

#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QScrollArea>
#include <QVBoxLayout>

#include "engine/core/instance/instance_utils.h"
#include "engine/core/util/fs_utils.h"
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

// -- Instance name prompt (Workspace-4fu) --

std::string prompt_instance_name(QWidget* parent, const QString& game_name) {
    const auto existing = engine::scan_instances();
    QString input = game_name;
    for (;;) {
        bool ok = false;
        input = QInputDialog::getText(
            parent, QObject::tr("Instance Name"),
            QObject::tr("Instance name:"),
            QLineEdit::Normal, input, &ok);
        if (!ok) return {};

        const std::string sanitized =
            engine::sanitize_directory_name(input.toStdString());
        if (sanitized.empty()) {
            QMessageBox::warning(parent, QObject::tr("Invalid Name"),
                                 QObject::tr("Please enter a valid name."));
            continue;
        }
        if (std::find(existing.begin(), existing.end(), sanitized) !=
            existing.end()) {
            QMessageBox::warning(
                parent, QObject::tr("Name In Use"),
                QObject::tr("An instance named \"%1\" already exists.")
                    .arg(QString::fromStdString(sanitized)));
            continue;
        }
        return sanitized;
    }
}

// -- GameSelectionWidget --

GameSelectionWidget::GameSelectionWidget(QWidget* parent)
    : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(40, 30, 40, 30);
    outer->setSpacing(12);

    // -- Header --
    auto* title = new QLabel(tr("Welcome to GameModManager"));
    QFont title_font = title->font();
    title_font.setPointSize(18);
    title_font.setBold(true);
    title->setFont(title_font);
    title->setAlignment(Qt::AlignCenter);
    outer->addWidget(title);

    auto* subtitle = new QLabel(tr("Select a game to manage"));
    subtitle->setObjectName("gameSelectionSubtitle");
    QFont sub_font = subtitle->font();
    sub_font.setPointSize(11);
    sub_font.setItalic(true);
    subtitle->setFont(sub_font);
    subtitle->setAlignment(Qt::AlignCenter);
    outer->addWidget(subtitle);

    // Divider between header area and list (Workspace-4fu).
    auto* divider = new QFrame();
    divider->setFrameShape(QFrame::HLine);
    divider->setFrameShadow(QFrame::Sunken);
    outer->addWidget(divider);

    // -- Filter bar --
    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText(tr("Filter..."));
    filter_->setClearButtonEnabled(true);
    outer->addWidget(filter_);
    connect(filter_, &QLineEdit::textChanged,
            this, &GameSelectionWidget::apply_filter);

    // -- Scrollable game list --
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    list_ = new QWidget();
    scroll->setWidget(list_);

    status_ = new QLabel(this);
    status_->setAlignment(Qt::AlignCenter);
    status_->setObjectName("gameSelectionNoMatch");
    status_->hide();

    outer->addWidget(scroll, 1);
    outer->addWidget(status_);

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

void GameSelectionWidget::apply_filter(const QString& text) {
    const QString needle = text.trimmed();
    bool any_visible = false;
    for (auto& card : cards_) {
        if (!card) continue;
        const bool match = needle.isEmpty() ||
            QString::fromStdString(card->entry().display_name)
                .contains(needle, Qt::CaseInsensitive);
        card->setVisible(match);
        any_visible = any_visible || match;
    }
    status_->setVisible(!any_visible);
    if (!any_visible) {
        status_->setText(tr("No games match \"%1\"").arg(needle));
    }
}

void GameSelectionWidget::set_games(const std::vector<GameEntry>& installed,
                                     const std::vector<GameEntry>& available) {
    // Cards are rebuilt below; drop the old QPointers and the old rows.
    cards_.clear();
    if (auto* old_layout = list_->layout()) {
        while (auto* item = old_layout->takeAt(0)) {
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }
        delete old_layout;
    }

    // One merged, alphabetically sorted list (Workspace-4fu). The game-less
    // "Generic Instance" entry (game_id "generic", no install path — creation
    // flows treat an empty path as valid) is always appended LAST.
    std::vector<GameEntry> all = installed;
    all.insert(all.end(), available.begin(), available.end());
    std::sort(all.begin(), all.end(),
              [](const GameEntry& a, const GameEntry& b) {
                  return QString::fromStdString(a.display_name).compare(
                             QString::fromStdString(b.display_name),
                             Qt::CaseInsensitive) < 0;
              });
    GameEntry generic;
    generic.game_id = "generic";
    generic.display_name = "Generic Instance";
    all.push_back(generic);

    auto* list_layout = new QVBoxLayout(list_);
    list_layout->setContentsMargins(0, 0, 0, 0);
    list_layout->setSpacing(2);
    for (const auto& entry : all) {
        auto* card = new GameCard(entry, list_);
        card->set_icon(resolve_icon(entry));
        cards_.push_back(card);
        connect(card, &GameCard::clicked, this, &GameSelectionWidget::game_selected);
        list_layout->addWidget(card);
    }
    list_layout->addStretch(1);

    apply_filter(filter_->text());
}

}  // namespace ui
