#include "ui/widgets/instance_switcher_dialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>
#include <QPainter>

#include "engine/plugin_host/plugin_loader.h"
#include "engine/instance/instance.h"
#include "ui/smooth_scroll.h"
#include "ui/settings/settings.h"

#include <filesystem>
#include <fstream>

namespace ui {

// Build a colored circle with the game's first letter
static QIcon make_game_icon(const std::string& game_id, const std::string& name) {
    auto hash = std::hash<std::string>{}(game_id);
    QColor base;
    switch (hash % 8) {
        case 0: base = QColor(100, 149, 237); break;
        case 1: base = QColor(220, 80, 80);   break;
        case 2: base = QColor(80, 180, 100);  break;
        case 3: base = QColor(200, 160, 60);  break;
        case 4: base = QColor(160, 100, 200); break;
        case 5: base = QColor(60, 180, 200);  break;
        case 6: base = QColor(220, 140, 60);  break;
        case 7: base = QColor(120, 120, 180); break;
    }

    QPixmap pm(48, 48);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(base);
    p.drawEllipse(2, 2, 44, 44);

    QString letter;
    if (!name.empty())
        letter = QString::fromStdString(name).left(1).toUpper();
    else
        letter = QString::fromStdString(game_id).left(1).toUpper();

    p.setPen(Qt::white);
    QFont f = p.font();
    f.setPointSize(18);
    f.setBold(true);
    p.setFont(f);
    p.drawText(QRect(0, 0, 48, 48), Qt::AlignCenter, letter);

    return QIcon(pm);
}

InstanceSwitcherDialog::InstanceSwitcherDialog(engine::PluginLoader* plugins, QWidget* parent)
    : QDialog(parent), plugins_(plugins) {
    setWindowTitle(tr("Switch Instance"));
    resize(520, 400);

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

    // Bottom buttons row
    auto* bottom_layout = new QHBoxLayout();
    bottom_layout->setContentsMargins(0, 0, 0, 0);

    auto* create_btn = new QPushButton(this);
    {
        auto icon = style()->standardIcon(QStyle::SP_FileDialogNewFolder);
        if (!icon.isNull()) {
            create_btn->setIcon(icon);
        }
        create_btn->setText(tr("Create new instance"));
    }
    bottom_layout->addWidget(create_btn);

    bottom_layout->addStretch();

    auto* ok_btn = new QPushButton(tr("OK"), this);
    auto* cancel_btn = new QPushButton(tr("Cancel"), this);
    ok_btn->setDefault(true);
    bottom_layout->addWidget(ok_btn);
    bottom_layout->addWidget(cancel_btn);

    main_layout->addLayout(bottom_layout);

    // Connections
    connect(ok_btn, &QPushButton::clicked, this, &InstanceSwitcherDialog::on_ok);
    connect(cancel_btn, &QPushButton::clicked, this, &QDialog::reject);
    connect(create_btn, &QPushButton::clicked, this, &InstanceSwitcherDialog::on_create);
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        on_ok();
    });

    // TODO: gate behind a Settings "Smooth scrolling" checkbox.
    if (Settings::instance().smooth_scrolling())
        ui::enable_smooth_scrolling(this);
}

void InstanceSwitcherDialog::load_instances(const std::string& instances_dir) {
    instances_dir_ = instances_dir;
    refresh_list();
}

void InstanceSwitcherDialog::refresh_list() {
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
        std::ifstream f(toml);
        std::string line;
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto key = line.substr(0, eq);
            key.erase(key.find_last_not_of(" \t") + 1);
            key.erase(0, key.find_first_not_of(" \t"));

            auto q1 = line.find('"', eq + 1);
            if (q1 == std::string::npos) continue;
            auto q2 = line.find('"', q1 + 1);
            if (q2 == std::string::npos) continue;
            auto val = line.substr(q1 + 1, q2 - q1 - 1);

            if (key == "game_id") ie.game_id = val;
            else if (key == "portable") ie.portable = (val == "true");
        }

        // Resolve display name from plugin
        std::string display_name;
        if (plugins_) {
            display_name = plugins_->display_name_for(ie.game_id);
        }
        if (display_name.empty()) {
            display_name = ie.game_id;
        }

        // Build the display label - always use the plugin's real display name
        // (the folder name has colons and other special chars stripped)
        std::string label = display_name;

        entries_.push_back(ie);

        // Create list item with custom widget
        auto* item_widget = new QWidget();
        auto* hlay = new QHBoxLayout(item_widget);
        hlay->setContentsMargins(8, 6, 8, 6);
        hlay->setSpacing(12);

        // Game icon
        auto* icon_label = new QLabel();
        auto icon = make_game_icon(ie.game_id, display_name);
        icon_label->setPixmap(icon.pixmap(36, 36));
        icon_label->setFixedSize(36, 36);
        icon_label->setAlignment(Qt::AlignCenter);
        hlay->addWidget(icon_label);

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

void InstanceSwitcherDialog::on_ok() {
    auto* item = list_->currentItem();
    if (!item) return;
    int row = list_->row(item);
    if (row >= 0 && row < static_cast<int>(entries_.size())) {
        selected_ = QString::fromStdString(entries_[row].name);
        accept();
    }
}

void InstanceSwitcherDialog::on_create() {
    create_requested_ = true;
    emit create_new_instance();
    accept();
}

}  // namespace ui
