#include "ui/modinfo/source_tab.h"

#include "engine/mod/meta/mod_meta.h"
#include "engine/source/nexus_provider.h"
#include "engine/source/source_provider.h"
#include "ui/theme/icon_manager.h"
#include "ui/modinfo/bbcode.h"
#include "ui/modinfo/source_fetch_worker.h"
#include "ui/settings/settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QTabWidget>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

#include <cctype>
#include <string>

namespace ui {

namespace {

engine::SourceProvider* find_provider(const QString& name) {
    std::string low = name.trimmed().toStdString();
    for (auto& c : low)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto* provider : engine::SourceRegistry::instance().providers()) {
        auto matches = [&low](const std::string& s) {
            std::string sl = s;
            for (auto& c : sl)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return sl == low;
        };
        if (matches(provider->source_type()) || matches(provider->display_name()))
            return provider;
    }
    return nullptr;
}

}  // namespace

SourceTab::SourceTab(QWidget* parent) : ModInfoTab(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    sources_ = new QTabWidget(this);
    sources_->setDocumentMode(true);
    layout->addWidget(sources_, 1);
}

SourceTab::~SourceTab() = default;

QString SourceTab::meta_value(const char* section, const char* key) const {
    return QString::fromStdString(current().load_meta().get(section, key));
}

void SourceTab::set_meta_value(const char* section, const char* key,
                               const QString& v) {
    auto meta = current().load_meta();
    const QString before = QString::fromStdString(meta.get(section, key));
    if (before == v) return;
    meta.set(section, key, v.toStdString());
    current().save_meta(meta);
}

QWidget* SourceTab::build_nexus_page(QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);

    auto* form = new QFormLayout();
    mod_id_ = new QLineEdit(page);
    mod_id_->setPlaceholderText(QStringLiteral("0"));
    form->addRow(tr("Mod ID:"), mod_id_);

    source_game_ = new QComboBox(page);
    form->addRow(tr("Source game:"), source_game_);

    version_ = new QLineEdit(page);
    form->addRow(tr("Version:"), version_);

    category_ = new QLineEdit(page);
    category_->setPlaceholderText(QStringLiteral("0"));
    form->addRow(tr("Category:"), category_);
    layout->addLayout(form);

    auto* buttons = new QHBoxLayout();
    refresh_ = new QPushButton(tr("Refresh"), page);
    visit_ = new QPushButton(tr("Visit on Nexus"), page);
    buttons->addWidget(refresh_);
    buttons->addWidget(visit_);

    if (Settings::instance().endorsement_integration()) {
        auto* endorse = new QPushButton(tr("Endorse"), page);
        QObject::connect(endorse, &QPushButton::clicked, this,
                         [this]() { on_visit(); });
        buttons->addWidget(endorse);
    }
    if (Settings::instance().tracked_integration()) {
        auto* track = new QPushButton(tr("Track"), page);
        QObject::connect(track, &QPushButton::clicked, this,
                         [this]() { on_visit(); });
        buttons->addWidget(track);
    }
    buttons->addStretch(1);
    layout->addLayout(buttons);

    auto* custom_row = new QHBoxLayout();
    custom_url_toggle_ = new QCheckBox(tr("Custom URL:"), page);
    custom_url_ = new QLineEdit(page);
    custom_url_->setEnabled(false);
    visit_custom_ = new QPushButton(tr("Visit"), page);
    visit_custom_->setEnabled(false);
    custom_row->addWidget(custom_url_toggle_);
    custom_row->addWidget(custom_url_, 1);
    custom_row->addWidget(visit_custom_);
    layout->addLayout(custom_row);

    description_ = new QTextBrowser(page);
    description_->setOpenExternalLinks(true);
    layout->addWidget(description_, 1);

    connect(mod_id_, &QLineEdit::editingFinished, this,
            &SourceTab::persist_fields);
    connect(version_, &QLineEdit::editingFinished, this,
            &SourceTab::persist_fields);
    connect(category_, &QLineEdit::editingFinished, this,
            &SourceTab::persist_fields);
    connect(custom_url_, &QLineEdit::editingFinished, this,
            &SourceTab::persist_custom_url);
    connect(custom_url_toggle_, &QCheckBox::toggled, this,
            &SourceTab::on_custom_url_toggled);
    connect(refresh_, &QPushButton::clicked, this, &SourceTab::on_refresh);
    connect(visit_, &QPushButton::clicked, this, &SourceTab::on_visit);
    connect(visit_custom_, &QPushButton::clicked, this,
            &SourceTab::on_visit_custom);

    return page;
}

QWidget* SourceTab::build_generic_page(engine::SourceProvider* provider,
                                       QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* form = new QFormLayout(page);

    auto* source_name = new QLabel(
        QString::fromStdString(provider->display_name()), page);
    source_name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Source:"), source_name);

    auto* id_label = new QLabel(
        current().source_id.isEmpty()
            ? tr("(not installed from this source)")
            : current().source_id,
        page);
    id_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Source ID:"), id_label);

    return page;
}

void SourceTab::set_mod(const ModInfoData& data) {
    populate();
    set_has_data(!data.source_id.isEmpty() ||
                 !meta_value("General", "version").isEmpty() ||
                 !meta_value("Nexusmods", "modid").isEmpty());
}

void SourceTab::populate() {
    QScopedValueRollback loading_rollback(loading_, true);

    // Rebuild the per-source sub-tabs: the set depends on the game's
    // supported sources, and each build reads the current mod's data.
    while (sources_->count() > 0) {
        QWidget* page = sources_->widget(0);
        sources_->removeTab(0);
        delete page;
    }
    mod_id_ = nullptr;
    source_game_ = nullptr;
    version_ = nullptr;
    category_ = nullptr;
    refresh_ = nullptr;
    visit_ = nullptr;
    custom_url_toggle_ = nullptr;
    custom_url_ = nullptr;
    visit_custom_ = nullptr;
    description_ = nullptr;

    const QStringList sources = current().supported_sources;
    if (sources.isEmpty()) {
        auto* hint = new QLabel(
            tr("No download sources are available for this game."), sources_);
        hint->setWordWrap(true);
        sources_->addTab(hint, tr("Sources"));
        return;
    }

    // One sub-tab per source. Branded source icons (Nexus Mods, LoversLab,
    // Steam Workshop) resolve from resources/icons/vendor/ via IconManager.
    auto add_source_tab = [this](QWidget* page, const QString& title,
                                 const QString& source_key) {
        const std::string vendor_key = engine::vendor_icon_key(source_key.toStdString());
        if (vendor_key.empty()) {
            sources_->addTab(page, title);
        } else {
            sources_->addTab(page, engine::IconManager::instance().resolve_icon(
                                       QString::fromStdString(vendor_key)),
                             title);
        }
    };

    for (const QString& name : sources) {
        auto* provider = find_provider(name);
        if (provider == nullptr) {
            auto* hint = new QLabel(
                tr("This source has no configurable settings."), sources_);
            hint->setWordWrap(true);
            add_source_tab(hint, name, name);
            continue;
        }
        QWidget* page =
            provider->source_type() == "nexus"
                ? build_nexus_page(sources_)
                : build_generic_page(provider, sources_);
        add_source_tab(page, name,
                       QString::fromStdString(provider->source_type()));
    }

    if (mod_id_ == nullptr) return;  // no Nexus page for this game

    mod_id_->setText(meta_value("Nexusmods", "modid"));
    if (mod_id_->text().isEmpty())
        mod_id_->setText(current().source_id);

    source_game_->clear();
    source_game_->addItem(current().nexus_domain, current().nexus_domain);
    source_game_->setEnabled(false);

    version_->setText(meta_value("General", "version"));
    if (version_->text().isEmpty()) version_->setText(current().version);

    category_->setText(meta_value("Nexusmods", "nexuscategory"));

    custom_url_toggle_->setChecked(
        meta_value("General", "hasCustomURL") == QStringLiteral("true"));
    custom_url_->setText(meta_value("General", "url"));
    custom_url_->setEnabled(custom_url_toggle_->isChecked());
    visit_custom_->setEnabled(custom_url_toggle_->isChecked() &&
                              !custom_url_->text().isEmpty());

    update_version_color();
    render_description();
}

void SourceTab::first_activation() {
    // Show the stored description only; the user opts into network I/O via
    // the Refresh button (MO2 refreshes eagerly, but that needs the API key
    // and blocks; we don't).
    populate();
}

void SourceTab::update_version_color() {
    if (version_ == nullptr) return;
    const QString version = meta_value("General", "version");
    const QString newest = meta_value("General", "newestversion");
    if (!version.isEmpty() && !newest.isEmpty() && version != newest) {
        version_->setStyleSheet(QStringLiteral("color: red;"));
        version_->setToolTip(tr("Newest version: %1").arg(newest));
    } else {
        version_->setStyleSheet(QString());
        version_->setToolTip(tr("No update available"));
    }
}

void SourceTab::render_description() {
    if (description_ == nullptr) return;
    const QString stored = meta_value("Nexusmods", "nexusdescription");
    if (stored.isEmpty()) {
        description_->setHtml(QStringLiteral(
            "<div style=\"text-align:center; color:grey; padding-top:24px;\">"
            "<p>No Nexus description stored for this mod. Press "
            "<b>Refresh</b> to fetch it live.</p></div>"));
        return;
    }
    description_->setHtml(
        QStringLiteral("<html><body style=\"font-family:sans-serif;\">%1</body></html>")
            .arg(bbcode_to_html(stored)));
}

void SourceTab::on_refresh() {
    if (!current().fetch_nexus_info) return;
    if (mod_id_ == nullptr) return;
    const QString id = mod_id_->text().trimmed();
    if (id.isEmpty() || id.toInt() <= 0) return;

    ++refresh_generation_;
    if (fetch_in_flight_) {
        // A refresh is already on the wire: coalesce — relaunch once it
        // lands, so rapid Re-clicks queue at most one follow-up fetch and a
        // stale result never writes meta.
        refresh_pending_ = true;
        return;
    }
    launch_fetch();
}

void SourceTab::launch_fetch() {
    fetch_in_flight_ = true;
    refresh_pending_ = false;
    refresh_mod_id_ = current().id;
    const quint64 gen = refresh_generation_;
    auto fetch = current().fetch_nexus_info;

    if (refresh_ != nullptr) {
        refresh_->setEnabled(false);
        refresh_->setText(tr("Fetching…"));
    }

    if (source_fetch_thread_ == nullptr) {
        source_fetch_thread_ = new ui::SourceFetchThread(this);
        connect(source_fetch_thread_->worker(), &ui::SourceFetchWorker::finished,
                this, &SourceTab::on_fetch_finished);
    }
    source_fetch_thread_->start(std::move(fetch), gen);
}

void SourceTab::on_fetch_finished(engine::ModInfoResult result,
                                  quint64 generation) {
    fetch_in_flight_ = false;
    if (refresh_ != nullptr) {
        refresh_->setEnabled(true);
        refresh_->setText(tr("Refresh"));
    }

    // Drop a result that no longer belongs: superseded by a newer Refresh, or
    // the user switched mods (prev/next) while the fetch was in flight.
    const bool stale =
        generation != refresh_generation_ || refresh_mod_id_ != current().id;
    const bool relaunch = refresh_pending_;
    refresh_pending_ = false;

    if (!stale) apply_fetch_result(result);
    if (relaunch) launch_fetch();
}

void SourceTab::apply_fetch_result(const engine::ModInfoResult& result) {
    if (!result.available) {
        render_description();
        return;
    }

    auto meta = current().load_meta();
    const QString name = QString::fromStdString(result.name);
    if (!name.isEmpty()) meta.set("General", "name", name.toStdString());
    if (!result.version.empty())
        meta.set("General", "version",
                 QString::fromStdString(result.version).toStdString());
    if (!result.newest_version.empty())
        meta.set("General", "newestversion",
                 QString::fromStdString(result.newest_version).toStdString());
    if (!result.category_id.empty())
        meta.set("Nexusmods", "nexuscategory",
                 QString::fromStdString(result.category_id).toStdString());
    if (!result.description.empty())
        meta.set("Nexusmods", "nexusdescription",
                 QString::fromStdString(result.description).toStdString());
    current().save_meta(meta);

    populate();
}

void SourceTab::on_visit() {
    if (mod_id_ == nullptr) return;
    const QString id = mod_id_->text().trimmed();
    if (id.isEmpty() || id.toInt() <= 0) return;
    if (!current().open_url) return;
    if (current().nexus_domain.isEmpty()) return;  // no plugin identity → no valid URL
    current().open_url(QStringLiteral("https://www.nexusmods.com/%1/mods/%2")
                           .arg(current().nexus_domain, id));
}

void SourceTab::on_visit_custom() {
    if (custom_url_ == nullptr) return;
    const QString url = custom_url_->text().trimmed();
    if (url.isEmpty()) return;
    if (!current().open_url) return;
    current().open_url(url);
}

void SourceTab::on_custom_url_toggled() {
    if (loading_) return;
    custom_url_->setEnabled(custom_url_toggle_->isChecked());
    visit_custom_->setEnabled(custom_url_toggle_->isChecked() &&
                              !custom_url_->text().isEmpty());
    set_meta_value("General", "hasCustomURL",
                   custom_url_toggle_->isChecked() ? QStringLiteral("true")
                                                   : QStringLiteral("false"));
}

void SourceTab::persist_fields() {
    if (loading_) return;
    if (mod_id_ == nullptr) return;
    set_meta_value("Nexusmods", "modid", mod_id_->text().trimmed());
    set_meta_value("General", "version", version_->text().trimmed());
    set_meta_value("Nexusmods", "nexuscategory", category_->text().trimmed());
}

void SourceTab::persist_custom_url() {
    if (loading_) return;
    if (custom_url_ == nullptr) return;
    set_meta_value("General", "url", custom_url_->text().trimmed());
    visit_custom_->setEnabled(custom_url_toggle_->isChecked() &&
                              !custom_url_->text().isEmpty());
}

void SourceTab::save_state() {
    persist_fields();
    persist_custom_url();
}

}  // namespace ui
