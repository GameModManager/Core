#include "ui/widgets/instance_switcher_content_widget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include "engine/core/instance/instance_utils.h"
#include "engine/core/instance/toml_utils.h"
#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "ui/settings/settings.h"
#include "ui/widgets/game_icon_cache.h"
#include "ui/widgets/smooth_scroll.h"

#include <filesystem>

namespace ui {

InstanceSwitcherContentWidget::InstanceSwitcherContentWidget(
    engine::PluginLoader* plugins, QWidget* parent)
    : QWidget(parent), plugins_(plugins) {
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(16, 16, 16, 12);
    main_layout->setSpacing(12);

    // Title
    auto* title = new QLabel(tr("Select an instance"));
    {
        QFont f = title->font();
        f.setPointSize(14);
        f.setBold(true);
        title->setFont(f);
    }
    main_layout->addWidget(title);

    // Instance list
    list_ = new QListWidget(this);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setSpacing(2);
    main_layout->addWidget(list_, 1);

    // Create button
    auto* create_btn = new QPushButton(this);
    {
        auto icon = style()->standardIcon(QStyle::SP_FileDialogNewFolder);
        if (!icon.isNull()) {
            create_btn->setIcon(icon);
        }
        create_btn->setText(tr("Create new instance"));
    }
    auto* bottom_layout = new QHBoxLayout();
    bottom_layout->setContentsMargins(0, 0, 0, 0);
    bottom_layout->addWidget(create_btn);
    bottom_layout->addStretch();
    main_layout->addLayout(bottom_layout);

    // Connections
    connect(create_btn, &QPushButton::clicked, this,
            &InstanceSwitcherContentWidget::create_new_instance);
    connect(list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { emit_selected(); });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem*) {
        if (immediate_switch_)
            emit_selected();
    });

    // Swap an async-downloaded icon into its row when it lands
    connect(&GameIconCache::instance(), &GameIconCache::icon_ready,
            this, &InstanceSwitcherContentWidget::update_icons_for);

    // TODO: gate behind a Settings "Smooth scrolling" checkbox.
    if (Settings::instance().smooth_scrolling())
        ui::enable_smooth_scrolling(this);
}

void InstanceSwitcherContentWidget::set_immediate_switch(bool enabled) {
    immediate_switch_ = enabled;
}

void InstanceSwitcherContentWidget::load_instances(
    const std::string& instances_dir) {
    instances_dir_ = instances_dir;
    refresh_list();
}

QString InstanceSwitcherContentWidget::selected_instance() const {
    auto* item = list_->currentItem();
    if (!item)
        return {};
    int row = list_->row(item);
    if (row >= 0 && row < static_cast<int>(entries_.size()))
        return QString::fromStdString(entries_[row].name);
    return {};
}

void InstanceSwitcherContentWidget::emit_selected() {
    const QString name = selected_instance();
    if (!name.isEmpty())
        emit instance_selected(name);
}

void InstanceSwitcherContentWidget::refresh_list() {
    list_->clear();
    entries_.clear();

    std::error_code ec;
    auto dir = std::filesystem::path(instances_dir_);
    if (!std::filesystem::is_directory(dir, ec)) return;

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_directory()) continue;
        auto toml = entry.path() / "instance.toml";
        if (!std::filesystem::exists(toml)) continue;

        InstanceSwitcherEntry ie;
        ie.name = entry.path().filename().string();
        ie.root = entry.path();

        // Parse instance.toml for game_id and portable flag
        if (auto tbl = engine::parse_instance_toml(toml)) {
            if (auto v = (*tbl)["game_id"].value<std::string>()) {
                ie.game_id = *v;
            }
            if (auto v = (*tbl)["portable"].value<bool>()) {
                ie.portable = *v;
            }
        }

        // Resolve display name from plugin
        std::string display_name;
        if (plugins_) {
            display_name = plugins_->display_name_for(ie.game_id);
        }
        if (display_name.empty()) {
            display_name = ie.game_id;
        }

        // Build the display label: the instance's own user-chosen name
        // (Workspace-l6w). The plugin's game display name would be identical
        // on every row once several instances of the same game exist; the
        // game name still drives the icon lookup below.
        std::string label = engine::instance_display_name(entry.path());

        ie.display_name = display_name;
        entries_.push_back(ie);

        // Create list item with custom widget
        auto* item_widget = new QWidget();
        auto* hlay = new QHBoxLayout(item_widget);
        hlay->setContentsMargins(8, 6, 8, 6);
        hlay->setSpacing(12);

        // Game icon - declared icon from the global cache, or a letter avatar
        // while the fetch is in flight (icon_ready() swaps it in).
        auto* icon_label = new QLabel();
        icon_label->setFixedSize(36, 36);
        icon_label->setAlignment(Qt::AlignCenter);
        auto icon = GameIconCache::instance().icon_for(
            QString::fromStdString(ie.game_id),
            QString::fromStdString(display_name), 36);
        icon_label->setPixmap(icon.pixmap(36, 36));
        hlay->addWidget(icon_label);
        entries_.back().icon_label = icon_label;

        // Text column
        auto* text_layout = new QVBoxLayout();
        text_layout->setContentsMargins(0, 0, 0, 0);
        text_layout->setSpacing(2);

        auto* name_label = new QLabel(QString::fromStdString(label));
        {
            QFont f = name_label->font();
            f.setPointSize(11);
            f.setBold(true);
            name_label->setFont(f);
        }
        text_layout->addWidget(name_label);

        auto* path_label = new QLabel(QString::fromStdString(ie.root.string()));
        path_label->setObjectName("pathLabel");
        path_label->setWordWrap(false);
        text_layout->addWidget(path_label);

        hlay->addLayout(text_layout, 1);

        auto* qitem = new QListWidgetItem(list_);
        qitem->setSizeHint(item_widget->sizeHint());
        list_->addItem(qitem);
        list_->setItemWidget(qitem, item_widget);
    }
}

void InstanceSwitcherContentWidget::update_icons_for(const QString& game_id) {
    std::string gid = game_id.toStdString();
    for (auto& entry : entries_) {
        if (entry.game_id == gid && entry.icon_label) {
            auto icon = GameIconCache::instance().icon_for(
                QString::fromStdString(entry.game_id),
                QString::fromStdString(entry.display_name), 36);
            entry.icon_label->setPixmap(icon.pixmap(36, 36));
        }
    }
}

}  // namespace ui