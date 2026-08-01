#include "ui/settings/source_pages.h"

#include "engine/log/logger.h"
#include "engine/nexus_auth.h"
#include "engine/source/nexus_provider.h"
#include "engine/source/source_provider.h"
#include "engine/source/steam_workshop_provider.h"
#include "ui/settings/settings.h"

#include <QCheckBox>
#include <QDateTime>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

namespace ui {

namespace {

// -- Nexus Mods ---------------------------------------------------------------

QWidget* build_nexus_page(QWidget* parent) {
    auto& auth = engine::NexusAuth::instance();
    const bool has_key = auth.has_api_key();
    auto& s = Settings::instance();

    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);

    auto* api_group = new QGroupBox(QWidget::tr("API Key"), page);
    auto* api_layout = new QVBoxLayout(api_group);

    auto* key_edit = new QLineEdit(api_group);
    key_edit->setEchoMode(QLineEdit::Password);
    key_edit->setPlaceholderText(QWidget::tr("Enter your Nexus Mods API key..."));
    if (has_key)
        key_edit->setText(QString::fromStdString(auth.get_api_key()));

    auto* key_row = new QHBoxLayout;
    key_row->addWidget(key_edit, 1);

    auto* save_btn = new QPushButton(has_key ? QWidget::tr("Update")
                                             : QWidget::tr("Save"), api_group);
    auto* clear_btn = new QPushButton(QWidget::tr("Clear"), api_group);
    clear_btn->setEnabled(has_key);
    key_row->addWidget(save_btn);
    key_row->addWidget(clear_btn);
    api_layout->addLayout(key_row);
    api_layout->addWidget(new QLabel(
        QWidget::tr("Get your key at "
                    "<a href='https://www.nexusmods.com/users/myaccount?tab=api'>"
                    "nexusmods.com/users/myaccount?tab=api</a>"), api_group));
    layout->addWidget(api_group);

    auto* rl_group = new QGroupBox(QWidget::tr("API Rate Limit"), page);
    auto* rl_layout = new QVBoxLayout(rl_group);
    auto* rl_label = new QLabel(rl_group);
    auto info = auth.get_rate_limit();
    if (info.daily_limit > 0 || info.hourly_limit > 0) {
        QString text;
        auto budget_line = [&](const QString& name, int remaining, int limit,
                               int64_t reset) {
            QString line = QWidget::tr("%1: <b>%2</b> / %3")
                .arg(name).arg(remaining).arg(limit);
            if (reset > 0) {
                QDateTime dt = QDateTime::fromSecsSinceEpoch(reset);
                line += QWidget::tr(" &nbsp;(resets %1)")
                    .arg(dt.toLocalTime().toString(Qt::TextDate));
            }
            return line;
        };
        if (info.hourly_limit > 0)
            text += budget_line(QWidget::tr("Hourly"), info.hourly_remaining,
                                info.hourly_limit, info.hourly_reset);
        if (info.daily_limit > 0) {
            if (!text.isEmpty()) text += "<br>";
            text += budget_line(QWidget::tr("Daily"), info.daily_remaining,
                                info.daily_limit, info.daily_reset);
        }
        if (info.last_updated > 0) {
            QDateTime lu = QDateTime::fromSecsSinceEpoch(info.last_updated);
            text += "<br>" + QWidget::tr("Last request: %1")
                .arg(lu.toLocalTime().toString(Qt::TextDate));
        }
        rl_label->setText(text);
    } else {
        rl_label->setText(
            QWidget::tr("No API requests made yet in this session."));
    }
    rl_layout->addWidget(rl_label);
    layout->addWidget(rl_group);

    auto* int_group = new QGroupBox(QWidget::tr("Integration"), page);
    auto* int_layout = new QVBoxLayout(int_group);
    auto* endorse_box =
        new QCheckBox(QWidget::tr("Endorse mods from the manager"), int_group);
    endorse_box->setChecked(s.endorsement_integration());
    auto* track_box =
        new QCheckBox(QWidget::tr("Track mods from the manager"), int_group);
    track_box->setChecked(s.tracked_integration());
    auto* cat_box = new QCheckBox(
        QWidget::tr("Apply Nexus category mappings"), int_group);
    cat_box->setChecked(s.category_mappings());
    int_layout->addWidget(endorse_box);
    int_layout->addWidget(track_box);
    int_layout->addWidget(cat_box);
    layout->addWidget(int_group);

    auto* counter_box = new QCheckBox(
        QWidget::tr("Hide the API counter in the UI"), page);
    counter_box->setChecked(s.hide_api_counter());
    layout->addWidget(counter_box);
    layout->addStretch(1);

    QObject::connect(save_btn, &QPushButton::clicked,
                     [&auth, key_edit, save_btn, clear_btn, page]() {
        QString key = key_edit->text().trimmed();
        if (key.isEmpty()) {
            QMessageBox::warning(page, QWidget::tr("API Key"),
                QWidget::tr("Enter your Nexus Mods API key or click Clear to "
                            "remove it."));
            return;
        }
        auth.set_api_key(key.toStdString());
        clear_btn->setEnabled(true);
        save_btn->setText(QWidget::tr("Update"));
        engine::Logger::instance().info("Nexus API key saved");
        QMessageBox::information(page, QWidget::tr("API Key"),
                                 QWidget::tr("API key saved successfully."));
    });
    QObject::connect(clear_btn, &QPushButton::clicked,
                     [&auth, key_edit, save_btn, clear_btn, page]() {
        auto reply = QMessageBox::question(
            page, QWidget::tr("Clear API Key"),
            QWidget::tr("Remove the stored Nexus Mods API key?"),
            QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            auth.clear_api_key();
            key_edit->clear();
            clear_btn->setEnabled(false);
            save_btn->setText(QWidget::tr("Save"));
            engine::Logger::instance().info("Nexus API key cleared");
        }
    });
    QObject::connect(endorse_box, &QCheckBox::toggled,
                     [&s](bool on) { s.set_endorsement_integration(on); });
    QObject::connect(track_box, &QCheckBox::toggled,
                     [&s](bool on) { s.set_tracked_integration(on); });
    QObject::connect(cat_box, &QCheckBox::toggled,
                     [&s](bool on) { s.set_category_mappings(on); });
    QObject::connect(counter_box, &QCheckBox::toggled,
                     [&s](bool on) { s.set_hide_api_counter(on); });

    return page;
}

// -- Steam Workshop -----------------------------------------------------------

QWidget* build_workshop_page(engine::SteamWorkshopProvider* provider,
                             QWidget* parent) {
    auto& s = Settings::instance();
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);

    auto* group = new QGroupBox(
        QWidget::tr("Steam Web API Rate Limit"), page);
    auto* form = new QFormLayout(group);

    auto* limit_spin = new QSpinBox(group);
    limit_spin->setRange(1, 10000);
    limit_spin->setValue(s.workshop_rate_limit_per_hour());
    limit_spin->setSuffix(QWidget::tr(" / hour"));
    limit_spin->setToolTip(QWidget::tr("Steam's Web API is free but throttled; "
                                       "requests over the limit are dropped."));

    form->addRow(QWidget::tr("Requests"), limit_spin);
    layout->addWidget(group);

    QObject::connect(limit_spin, &QSpinBox::valueChanged,
                     [&s, provider](int v) {
        s.set_workshop_rate_limit_per_hour(v);
        provider->set_rate_limit(v, provider->rate_window());
    });

    layout->addStretch(1);
    return page;
}

} // namespace

QWidget* build_source_settings_page(engine::SourceProvider* provider,
                                    QWidget* parent) {
    if (provider == nullptr) return nullptr;
    if (provider->source_type() == "nexus") {
        return build_nexus_page(parent);
    }
    if (provider->source_type() == "steam") {
        auto* ws = dynamic_cast<engine::SteamWorkshopProvider*>(provider);
        if (ws != nullptr) return build_workshop_page(ws, parent);
    }
    return nullptr;
}

} // namespace ui
