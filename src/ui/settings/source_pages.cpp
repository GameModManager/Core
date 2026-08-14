#include "ui/settings/source_pages.h"

#include "engine/core/log/logger.h"
#include "engine/source/loverslab_auth.h"
#include "engine/source/nexus_auth.h"
#include "engine/source/nexus_account.h"
#include "engine/source/nexus_provider.h"
#include "engine/source/nexus_servers.h"
#include "engine/source/source_provider.h"
#include "engine/source/steam_workshop_provider.h"
#include "ui/settings/settings.h"

#ifdef GMM_PLATFORM_LINUX
#include "platform/linux/linux_platform.h"
#endif

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace ui {

namespace {

bool contains_ci(const std::string& haystack, const std::string& needle) {
    std::string h = haystack, n = needle;
    for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}

// MO2's localizedByteSpeed: e.g. "1.2 MB/s" / "340 KB/s".
QString format_speed(int bps) {
    if (bps <= 0) return {};
    const double kb = static_cast<double>(bps) / 1024.0;
    if (kb >= 1024.0)
        return QString::number(kb / 1024.0, 'f', 1) + " MB/s";
    return QString::number(kb, 'f', 0) + " KB/s";
}

QString account_type_label(engine::NexusUserInfo::AccountType t) {
    switch (t) {
        case engine::NexusUserInfo::AccountType::Premium:
            return QObject::tr("Premium");
        case engine::NexusUserInfo::AccountType::Supporter:
            return QObject::tr("Supporter");
        case engine::NexusUserInfo::AccountType::Regular:
            return QObject::tr("Regular");
        default:
            return QObject::tr("N/A");
    }
}

} // namespace

bool nexus_queue_default_for(engine::NexusUserInfo::AccountType type) {
    // Supporter keeps the free-account ~1.5MB/s throttle (only Premium lifts
    // it), so only a Premium account defaults to parallel Nexus downloads.
    return type != engine::NexusUserInfo::AccountType::Premium;
}

void apply_nexus_queue_default() {
    auto& s = Settings::instance();
    // The user's explicit choice wins - the tier default is applied only
    // while the value has never been set manually.
    if (s.nexus_queue_downloads_set()) return;
    auto& auth = engine::NexusAuth::instance();
    const auto type = auth.has_user_info()
        ? auth.get_user_info().account_type
        : engine::NexusUserInfo::AccountType::None;
    s.set_nexus_queue_downloads(nexus_queue_default_for(type));
}

// -- NexusManualKeyDialog ----------------------------------------------------

NexusManualKeyDialog::NexusManualKeyDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Enter API Key Manually"));

    auto* layout = new QVBoxLayout(this);

    key_edit_ = new QPlainTextEdit(this);
    key_edit_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    key_edit_->setPlaceholderText(tr("Paste your Nexus Mods API key here."));
    layout->addWidget(key_edit_);

    auto* open_browser_btn = new QPushButton(tr("Open Browser"), this);
    auto* paste_btn = new QPushButton(tr("Paste"), this);
    auto* clear_btn = new QPushButton(tr("Clear"), this);
    auto* btn_row = new QHBoxLayout;
    btn_row->addWidget(open_browser_btn);
    btn_row->addWidget(paste_btn);
    btn_row->addWidget(clear_btn);
    layout->addLayout(btn_row);

    auto* button_box =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(button_box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(button_box);

    connect(open_browser_btn, &QPushButton::clicked, this, [this] { open_browser(); });
    connect(paste_btn, &QPushButton::clicked, this, [this] { paste(); });
    connect(clear_btn, &QPushButton::clicked, this, [this] { clear(); });
}

QString NexusManualKeyDialog::key() const {
    return key_;
}

void NexusManualKeyDialog::accept() {
    key_ = key_edit_->toPlainText();
    QDialog::accept();
}

void NexusManualKeyDialog::open_browser() {
    QDesktopServices::openUrl(
        QUrl("https://www.nexusmods.com/users/myaccount?tab=api"));
}

void NexusManualKeyDialog::paste() {
    const QString text = QApplication::clipboard()->text();
    if (!text.isEmpty()) key_edit_->setPlainText(text);
}

void NexusManualKeyDialog::clear() {
    key_edit_->clear();
}

// -- NexusPanel --------------------------------------------------------------

// The Settings -> Sources -> Nexus sub-tab. A 1:1 clone of MO2's
// settingsdialog.ui nexus tab (Nexus Account / Statistics / Nexus Connection /
// Options / Servers). Unlike MO2 we use the API-key flow for everything (the
// "Connect" button validates the stored key against users/validate.json), so
// there is no OAuth login dialog.
class NexusPanel : public QWidget {
public:
    explicit NexusPanel(QWidget* parent = nullptr);

private:
    void add_log(const QString& s);
    void log_clear();
    void refresh_account_and_stats();
    void refresh_buttons();
    void refresh_servers();
    QListWidgetItem* make_server_item(const engine::NexusServer& srv) const;
    void persist_server_preferences();

    void connect_to_nexus();
    void enter_key_manually();
    void disconnect_from_nexus();
    void associate_with_nxm();

    QLabel* nexus_user_id_ = nullptr;
    QLabel* nexus_name_ = nullptr;
    QLabel* nexus_account_ = nullptr;
    QLabel* nexus_daily_ = nullptr;
    QLabel* nexus_hourly_ = nullptr;

    QPushButton* connect_btn_ = nullptr;
    QPushButton* manual_btn_ = nullptr;
    QPushButton* disconnect_btn_ = nullptr;
    QPushButton* associate_btn_ = nullptr;

    QListWidget* log_list_ = nullptr;
    QListWidget* known_list_ = nullptr;
    QListWidget* preferred_list_ = nullptr;

    // Persisting server preferences on every model change would wipe them
    // while the lists are being (re)built during construction - the model
    // change signals fire on clear()/addItem(). Only start persisting once
    // the initial population is done.
    bool servers_ready_ = false;
};

NexusPanel::NexusPanel(QWidget* parent)
    : QWidget(parent) {
    auto& auth = engine::NexusAuth::instance();
    auto& s = Settings::instance();

    auto* outer = new QVBoxLayout(this);

    // -- Box 1: Nexus Account | Statistics -----------------------------------
    auto* box1 = new QHBoxLayout;
    auto* account_group = new QGroupBox(tr("Nexus Account"), this);
    auto* account_form = new QFormLayout(account_group);
    nexus_user_id_ = new QLabel(tr("N/A"), account_group);
    nexus_user_id_->setObjectName("nexusUserID");
    nexus_name_ = new QLabel(tr("N/A"), account_group);
    nexus_name_->setObjectName("nexusName");
    nexus_account_ = new QLabel(tr("N/A"), account_group);
    nexus_account_->setObjectName("nexusAccount");
    account_form->addRow(tr("User ID:"), nexus_user_id_);
    account_form->addRow(tr("Name:"), nexus_name_);
    account_form->addRow(tr("Account:"), nexus_account_);
    box1->addWidget(account_group);

    auto* stats_group = new QGroupBox(tr("Statistics"), this);
    auto* stats_form = new QFormLayout(stats_group);
    nexus_daily_ = new QLabel(tr("N/A"), stats_group);
    nexus_daily_->setObjectName("nexusDailyRequests");
    nexus_hourly_ = new QLabel(tr("N/A"), stats_group);
    nexus_hourly_->setObjectName("nexusHourlyRequests");
    stats_form->addRow(tr("Daily requests:"), nexus_daily_);
    stats_form->addRow(tr("Hourly requests:"), nexus_hourly_);
    box1->addWidget(stats_group);
    outer->addLayout(box1);

    // -- Box 2: Nexus Connection ----------------------------------------------
    auto* conn_group = new QGroupBox(tr("Nexus Connection"), this);
    auto* conn_layout = new QHBoxLayout(conn_group);
    auto* left_col = new QVBoxLayout;
    connect_btn_ = new QPushButton(tr("Connect to Nexus"), conn_group);
    connect_btn_->setObjectName("nexusConnect");
    connect_btn_->setEnabled(false);
    connect_btn_->setToolTip(
        tr("Disabled: account validation currently crashes the app (see implementation.md Log)."));
    manual_btn_ = new QPushButton(tr("Enter API Key Manually"), conn_group);
    manual_btn_->setObjectName("nexusManualKey");
    manual_btn_->setToolTip(tr("Manually enter the API key and try to login"));
    disconnect_btn_ = new QPushButton(tr("Disconnect from Nexus"), conn_group);
    disconnect_btn_->setObjectName("nexusDisconnect");
    disconnect_btn_->setToolTip(tr("Clear the stored Nexus authorization."));
    left_col->addWidget(connect_btn_);
    left_col->addWidget(manual_btn_);
    left_col->addWidget(disconnect_btn_);
    left_col->addStretch(1);
    conn_layout->addLayout(left_col, 1);

    log_list_ = new QListWidget(conn_group);
    log_list_->setObjectName("nexusLog");
    conn_layout->addWidget(log_list_, 9);
    outer->addWidget(conn_group);

    // -- Box 3: Options --------------------------------------------------------
    auto* options_group = new QGroupBox(tr("Options"), this);
    auto* options_layout = new QHBoxLayout(options_group);
    auto* opts_left = new QVBoxLayout;
    auto* endorse_box = new QCheckBox(tr("Endorsement Integration"), options_group);
    endorse_box->setChecked(s.endorsement_integration());
    auto* track_box = new QCheckBox(tr("Tracked Integration"), options_group);
    track_box->setChecked(s.tracked_integration());
    auto* cat_box = new QCheckBox(tr("Apply Nexus category mappings"), options_group);
    cat_box->setChecked(s.category_mappings());
    cat_box->setEnabled(false);  // work in progress - kept, but inert
    cat_box->setToolTip(tr("Work in progress"));
    auto* counter_box = new QCheckBox(tr("Hide API Request Counter"), options_group);
    counter_box->setChecked(s.hide_api_counter());
    // Queue Nexus downloads one-at-a-time. The initial state is the effective
    // value: the stored one once the user chose it, otherwise the tier-derived
    // default (Regular/Supporter -> queue, Premium -> parallel). Wiring happens
    // below so the construction-time setChecked never writes a stored value.
    auto* queue_box = new QCheckBox(tr("Queue downloads (one at a time)"), options_group);
    queue_box->setObjectName("nexusQueueDownloads");
    queue_box->setToolTip(
        tr("Free Regular/Supporter accounts are throttled to ~1.5MB/s, so "
           "parallel downloads don't help; Premium lifts the cap."));
    const bool queue_effective = s.nexus_queue_downloads_set()
        ? s.nexus_queue_downloads()
        : nexus_queue_default_for(auth.has_user_info()
                                      ? auth.get_user_info().account_type
                                      : engine::NexusUserInfo::AccountType::None);
    queue_box->setChecked(queue_effective);
    opts_left->addWidget(endorse_box);
    opts_left->addWidget(track_box);
    opts_left->addWidget(cat_box);
    opts_left->addWidget(counter_box);
    opts_left->addWidget(queue_box);
    options_layout->addLayout(opts_left, 1);

    auto* opts_right = new QVBoxLayout;
    associate_btn_ = new QPushButton(
        tr("Associate with \"Download with manager\" links"), options_group);
    associate_btn_->setObjectName("associateButton");
    opts_right->addWidget(associate_btn_);
    opts_right->addStretch(1);
    options_layout->addLayout(opts_right, 1);
    outer->addWidget(options_group);

    // -- Box 4: Servers ---------------------------------------------------------
    auto* servers_group = new QGroupBox(tr("Servers"), this);
    auto* servers_layout = new QHBoxLayout(servers_group);
    auto* known_col = new QVBoxLayout;
    known_col->addWidget(new QLabel(
        tr("Known Servers (updated on download)"), servers_group));
    known_list_ = new QListWidget(servers_group);
    known_list_->setObjectName("knownServersList");
    known_list_->setDragDropMode(QAbstractItemView::DragDrop);
    known_list_->setDefaultDropAction(Qt::MoveAction);
    known_col->addWidget(known_list_);
    servers_layout->addLayout(known_col, 1);

    auto* preferred_col = new QVBoxLayout;
    preferred_col->addWidget(new QLabel(
        tr("Preferred Servers (Drag & Drop)"), servers_group));
    preferred_list_ = new QListWidget(servers_group);
    preferred_list_->setObjectName("preferredServersList");
    preferred_list_->setDragDropMode(QAbstractItemView::DragDrop);
    preferred_list_->setDefaultDropAction(Qt::MoveAction);
    preferred_col->addWidget(preferred_list_);
    servers_layout->addLayout(preferred_col, 1);
    outer->addWidget(servers_group);

    outer->addStretch(1);

    // -- Wiring ---------------------------------------------------------------
    connect(connect_btn_, &QPushButton::clicked, this, [this] { connect_to_nexus(); });
    connect(manual_btn_, &QPushButton::clicked, this, [this] { enter_key_manually(); });
    connect(disconnect_btn_, &QPushButton::clicked, this,
            [this] { disconnect_from_nexus(); });
    connect(associate_btn_, &QPushButton::clicked, this,
            [this] { associate_with_nxm(); });

    connect(endorse_box, &QCheckBox::toggled,
            [&s](bool on) { s.set_endorsement_integration(on); });
    connect(track_box, &QCheckBox::toggled,
            [&s](bool on) { s.set_tracked_integration(on); });
    connect(counter_box, &QCheckBox::toggled,
            [&s](bool on) { s.set_hide_api_counter(on); });
    connect(queue_box, &QCheckBox::toggled,
            [&s](bool on) { s.set_nexus_queue_downloads(on); });
    // category_mappings is intentionally not wired: the checkbox is disabled
    // (work in progress) and must never change the stored value.

    auto persist = [this] { persist_server_preferences(); };
    connect(known_list_->model(), &QAbstractItemModel::rowsMoved, this, persist);
    connect(known_list_->model(), &QAbstractItemModel::rowsInserted, this, persist);
    connect(known_list_->model(), &QAbstractItemModel::rowsRemoved, this, persist);
    connect(preferred_list_->model(), &QAbstractItemModel::rowsMoved, this, persist);
    connect(preferred_list_->model(), &QAbstractItemModel::rowsInserted, this, persist);
    connect(preferred_list_->model(), &QAbstractItemModel::rowsRemoved, this, persist);

    // -- Initial state ---------------------------------------------------------
    if (auth.has_api_key())
        add_log(tr("Connected."));
    else
        add_log(tr("Not connected."));

    refresh_account_and_stats();
    refresh_buttons();
    refresh_servers();
    servers_ready_ = true;
}

void NexusPanel::add_log(const QString& s) {
    log_list_->addItem(s);
    log_list_->scrollToBottom();
}

void NexusPanel::log_clear() {
    log_list_->clear();
}

void NexusPanel::refresh_account_and_stats() {
    auto& auth = engine::NexusAuth::instance();

    if (auth.has_user_info()) {
        const auto info = auth.get_user_info();
        nexus_user_id_->setText(QString::fromStdString(info.user_id));
        nexus_name_->setText(QString::fromStdString(info.name));
        nexus_account_->setText(account_type_label(info.account_type));
    } else {
        nexus_user_id_->setText(tr("N/A"));
        nexus_name_->setText(tr("N/A"));
        nexus_account_->setText(tr("N/A"));
    }

    const auto rl = auth.get_rate_limit();
    if (rl.daily_limit > 0)
        nexus_daily_->setText(QString("%1/%2").arg(rl.daily_remaining).arg(rl.daily_limit));
    else
        nexus_daily_->setText(tr("N/A"));
    if (rl.hourly_limit > 0)
        nexus_hourly_->setText(QString("%1/%2").arg(rl.hourly_remaining).arg(rl.hourly_limit));
    else
        nexus_hourly_->setText(tr("N/A"));
}

void NexusPanel::refresh_buttons() {
    const bool has_key = engine::NexusAuth::instance().has_api_key();
    // connect_btn_ stays permanently disabled: validate_nexus_account() soft-crashes
    // the app (see implementation.md Log 2026-08-01).
    disconnect_btn_->setEnabled(has_key);
#ifdef GMM_PLATFORM_LINUX
    associate_btn_->setEnabled(!engine::LinuxPlatform::is_nxm_handler_registered());
#else
    associate_btn_->setEnabled(false);
#endif
}

QListWidgetItem* NexusPanel::make_server_item(const engine::NexusServer& srv) const {
    QString text = QString::fromStdString(srv.name);
    if (contains_ci(srv.name, "CDN")) text += tr(" (automatic)");
    const int avg = srv.average_speed();
    if (avg > 0) text += QString(" (%1)").arg(format_speed(avg));
    auto* item = new QListWidgetItem(text);
    item->setData(Qt::UserRole, QString::fromStdString(srv.name));
    return item;
}

void NexusPanel::refresh_servers() {
    known_list_->clear();
    preferred_list_->clear();
    for (const auto& srv : engine::NexusServers::instance().known())
        known_list_->addItem(make_server_item(srv));
    for (const auto& srv : engine::NexusServers::instance().preferred())
        preferred_list_->addItem(make_server_item(srv));
}

void NexusPanel::persist_server_preferences() {
    if (!servers_ready_) return;
    std::vector<std::string> ordered;
    ordered.reserve(preferred_list_->count());
    for (int i = 0; i < preferred_list_->count(); ++i) {
        const QString name = preferred_list_->item(i)->data(Qt::UserRole).toString();
        if (!name.isEmpty()) ordered.push_back(name.toStdString());
    }
    engine::NexusServers::instance().set_preferred(ordered);
}

void NexusPanel::connect_to_nexus() {
    log_clear();
    add_log(tr("Authorizing with Nexus..."));
    const auto r = engine::validate_nexus_account();
    if (r.ok) {
        add_log(tr("Received user account information"));
        add_log(tr("Linked with Nexus successfully."));
        // First login for this setting: seed the queue-downloads default from
        // the account tier (Regular/Supporter -> queue, Premium -> parallel).
        // A value the user set manually is never overridden.
        apply_nexus_queue_default();
    } else {
        add_log(QString::fromStdString(r.message));
    }
    refresh_account_and_stats();
    refresh_buttons();
}

void NexusPanel::enter_key_manually() {
    NexusManualKeyDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;

    const QString key = dlg.key().trimmed();
    if (key.isEmpty()) {
        // MO2 parity: an empty key clears the stored credentials.
        disconnect_from_nexus();
        return;
    }
    engine::NexusAuth::instance().set_api_key(key.toStdString());
    engine::Logger::instance().info("Nexus API key stored");
    connect_to_nexus();
}

void NexusPanel::disconnect_from_nexus() {
    auto& auth = engine::NexusAuth::instance();
    auth.clear_api_key();
    auth.clear_user_info();
    log_clear();
    add_log(tr("Disconnected."));
    engine::Logger::instance().info("Nexus API key cleared");
    refresh_account_and_stats();
    refresh_buttons();
}

void NexusPanel::associate_with_nxm() {
#ifdef GMM_PLATFORM_LINUX
    const auto app_path = std::filesystem::path(
        QCoreApplication::applicationFilePath().toStdString());
    if (engine::LinuxPlatform::register_nxm_handler(app_path)) {
        if (engine::LinuxPlatform::register_gmm_handler(app_path))
            Settings::instance().set_nxm_handler_check("dont_ask");
        else
            engine::Logger::instance().error(
                "Failed to register GameModManager as an x-scheme-handler for nxm://");
        add_log(tr("Associated GameModManager with \"Download with manager\" links."));
    } else {
        add_log(tr("Failed to register the nxm:// handler."));
    }
#else
    add_log(tr("nxm:// registration is not supported on this platform."));
#endif
    refresh_buttons();
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

// -- LoversLab ----------------------------------------------------------------

// Settings -> Sources -> LoversLab. LoversLab has no public API, so downloads
// ride the user's browser session: paste the site's cookies (name=value pairs
// joined by "; ") and GMM sends them as the Cookie header when fetching a
// ?do=download link. The cookie is a session secret - it lives in the OS
// keyring (same backend as the Nexus API key), never on disk in plain text.
QWidget* build_loverslab_page(QWidget* parent) {
    auto& auth = engine::LoversLabAuth::instance();
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);

    auto* group = new QGroupBox(QWidget::tr("Session Cookie"), page);
    auto* v = new QVBoxLayout(group);

    auto* hint = new QLabel(
        QWidget::tr("LoversLab has no public API, so GMM downloads with the "
                    "session of a signed-in browser tab.\n\n"
                    "How to get the cookie:\n"
                    "1. Sign in to loverslab.com in your browser.\n"
                    "2. Open the developer tools (F12) -> Network tab, reload "
                    "the page, click any request, and copy the \"Cookie\" "
                    "request header.\n"
                    "3. Paste it below and press Save.\n\n"
                    "Any format is fine (name=value pairs, or the browser's "
                    "tab-separated cookie list) - GMM normalizes it on save. "
                    "The cookie is a session secret - it is stored in your OS "
                    "keyring and never shown back to you."), group);
    hint->setWordWrap(true);
    v->addWidget(hint);

    auto* cookie_edit = new QLineEdit(group);
    cookie_edit->setEchoMode(QLineEdit::Password);
    cookie_edit->setPlaceholderText(
        QWidget::tr("memberID=...; pass_hash=...; ..."));
    if (auth.has_cookie()) {
        cookie_edit->setText(QString::fromStdString(auth.get_cookie()));
    }
    v->addWidget(cookie_edit);

    auto* status = new QLabel(group);
    status->setObjectName("llCookieStatus");
    status->setText(auth.has_cookie()
        ? QWidget::tr("Stored: %1").arg(QString::fromStdString(
              engine::LoversLabAuth::redact(auth.get_cookie())))
        : QWidget::tr("No cookie stored."));
    v->addWidget(status);

    auto* btn_row = new QHBoxLayout;
    auto* paste_btn = new QPushButton(QWidget::tr("Paste"), group);
    auto* save_btn = new QPushButton(QWidget::tr("Save"), group);
    auto* clear_btn = new QPushButton(QWidget::tr("Clear"), group);
    btn_row->addWidget(paste_btn);
    btn_row->addWidget(save_btn);
    btn_row->addWidget(clear_btn);
    v->addLayout(btn_row);

    layout->addWidget(group);
    layout->addStretch(1);

    auto update_status = [&auth, status](const std::string& cookie) {
        status->setText(cookie.empty()
            ? QWidget::tr("No cookie stored.")
            : QWidget::tr("Stored: %1").arg(QString::fromStdString(
                  engine::LoversLabAuth::redact(cookie))));
    };

    QObject::connect(paste_btn, &QPushButton::clicked,
                     [cookie_edit]() {
        const QString text = QApplication::clipboard()->text();
        if (!text.isEmpty()) cookie_edit->setText(text);
    });
    QObject::connect(save_btn, &QPushButton::clicked,
                     [&auth, cookie_edit, update_status]() {
        const std::string cookie = cookie_edit->text().trimmed().toStdString();
        if (cookie.empty()) {
            auth.clear_cookie();
        } else {
            auth.set_cookie(cookie);
        }
        update_status(cookie);
    });
    QObject::connect(clear_btn, &QPushButton::clicked,
                     [&auth, cookie_edit, update_status]() {
        auth.clear_cookie();
        cookie_edit->clear();
        update_status({});
    });

    return page;
}

QWidget* build_source_settings_page(engine::SourceProvider* provider,
                                    QWidget* parent) {
    if (provider == nullptr) return nullptr;
    if (provider->source_type() == "nexus") {
        return new NexusPanel(parent);
    }
    if (provider->source_type() == "steam") {
        auto* ws = dynamic_cast<engine::SteamWorkshopProvider*>(provider);
        if (ws != nullptr) return build_workshop_page(ws, parent);
    }
    if (provider->source_type() == "loverslab") {
        return build_loverslab_page(parent);
    }
    return nullptr;
}

} // namespace ui
