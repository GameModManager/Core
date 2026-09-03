#include "ui/modinfo/source_panels/nexus_source_panel.h"

#include "ui/modinfo/bbcode.h"
#include "ui/modinfo/description_browser.h"
#include "ui/modinfo/source_fetch_worker.h"
#include "ui/settings/settings.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QScopedValueRollback>
#include <QVBoxLayout>

namespace ui {

NexusSourcePanel::NexusSourcePanel(const ModInfoData &data, QWidget *parent)
    : SourceInfoPanel(data, parent) {
  auto *layout = new QVBoxLayout(this);

  auto *form = new QFormLayout();
  mod_id_ = new QLineEdit(this);
  mod_id_->setPlaceholderText(QStringLiteral("0"));
  form->addRow(tr("Mod ID:"), mod_id_);

  source_game_ = new QComboBox(this);
  form->addRow(tr("Source game:"), source_game_);

  version_ = new QLineEdit(this);
  form->addRow(tr("Version:"), version_);

  category_ = new QLineEdit(this);
  category_->setPlaceholderText(QStringLiteral("0"));
  form->addRow(tr("Category:"), category_);
  layout->addLayout(form);

  auto *buttons = new QHBoxLayout();
  refresh_ = new QPushButton(tr("Refresh"), this);
  visit_ = new QPushButton(tr("Visit on Nexus"), this);
  buttons->addWidget(refresh_);
  buttons->addWidget(visit_);

  if (Settings::instance().endorsement_integration()) {
    auto *endorse = new QPushButton(tr("Endorse"), this);
    connect(endorse, &QPushButton::clicked, this, [this]() { on_visit(); });
    buttons->addWidget(endorse);
  }
  if (Settings::instance().tracked_integration()) {
    auto *track = new QPushButton(tr("Track"), this);
    connect(track, &QPushButton::clicked, this, [this]() { on_visit(); });
    buttons->addWidget(track);
  }
  buttons->addStretch(1);
  layout->addLayout(buttons);

  auto *custom_row = new QHBoxLayout();
  custom_url_toggle_ = new QCheckBox(tr("Custom URL:"), this);
  custom_url_ = new QLineEdit(this);
  custom_url_->setEnabled(false);
  visit_custom_ = new QPushButton(tr("Visit"), this);
  visit_custom_->setEnabled(false);
  custom_row->addWidget(custom_url_toggle_);
  custom_row->addWidget(custom_url_, 1);
  custom_row->addWidget(visit_custom_);
  layout->addLayout(custom_row);

  description_ = new DescriptionBrowser(this);
  description_->setOpenExternalLinks(true);
  layout->addWidget(description_, 1);

  connect(mod_id_, &QLineEdit::editingFinished, this,
          &NexusSourcePanel::persist_fields);
  connect(version_, &QLineEdit::editingFinished, this,
          &NexusSourcePanel::persist_fields);
  connect(category_, &QLineEdit::editingFinished, this,
          &NexusSourcePanel::persist_fields);
  connect(custom_url_, &QLineEdit::editingFinished, this,
          &NexusSourcePanel::persist_custom_url);
  connect(custom_url_toggle_, &QCheckBox::toggled, this,
          &NexusSourcePanel::on_custom_url_toggled);
  connect(refresh_, &QPushButton::clicked, this, &NexusSourcePanel::on_refresh);
  connect(visit_, &QPushButton::clicked, this, &NexusSourcePanel::on_visit);
  connect(visit_custom_, &QPushButton::clicked, this,
          &NexusSourcePanel::on_visit_custom);

  populate();
}

NexusSourcePanel::~NexusSourcePanel() = default;

void NexusSourcePanel::populate() {
  QScopedValueRollback<bool> guard(loading_, true);

  mod_id_->setText(meta_value("Nexusmods", "modid"));
  if (mod_id_->text().isEmpty())
    mod_id_->setText(data_.source_id);

  source_game_->clear();
  source_game_->addItem(data_.nexus_domain, data_.nexus_domain);
  source_game_->setEnabled(false);

  version_->setText(meta_value("General", "version"));
  if (version_->text().isEmpty())
    version_->setText(data_.version);

  category_->setText(meta_value("Nexusmods", "nexuscategory"));

  custom_url_toggle_->setChecked(meta_value("General", "hasCustomURL") ==
                                 QStringLiteral("true"));
  custom_url_->setText(meta_value("General", "url"));
  custom_url_->setEnabled(custom_url_toggle_->isChecked());
  visit_custom_->setEnabled(custom_url_toggle_->isChecked() &&
                            !custom_url_->text().isEmpty());

  update_version_color();
  render_description();
}

bool NexusSourcePanel::has_data() const {
  // has_data gates the Source tab's red-dot in the mod list (Workspace-rvld):
  // returning true just because a version string exists caused EVERY mod -
  // including manual / Steam / LoversLab installs, all of which get a
  // "1.0" default version - to report "Nexus has data", which polluted the
  // Source-tab visibility check. The Nexus panel truly has data only when
  // the mod is actually Nexus-sourced:
  //   - data_.source_type is "nexus" (set by load_meta_for_mods), OR
  //   - a real Nexus mod id is stored in [Nexusmods] (numeric, > "0").
  // "0" / empty are MO2's "no Nexus id" sentinels and must NOT count.
  if (data_.source_type == QLatin1String("nexus") && !data_.source_id.isEmpty())
    return true;
  const QString modid = meta_value("Nexusmods", "modid");
  if (!modid.isEmpty() && modid != QLatin1String("0"))
    return modid.toLongLong() > 0;
  return false;
}

void NexusSourcePanel::save_state() {
  persist_fields();
  persist_custom_url();
}

void NexusSourcePanel::update_version_color() {
  if (version_ == nullptr)
    return;
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

void NexusSourcePanel::render_description() {
  if (description_ == nullptr)
    return;
  // Drop any in-flight image fetches and resource cache from the
  // previous render - the next mod's description is unrelated and
  // late-arriving bytes would race the new document.
  description_->clear_image_cache();
  const QString stored = meta_value("Nexusmods", "nexusdescription");
  if (stored.isEmpty()) {
    description_->setHtml(QStringLiteral(
        "<div style=\"text-align:center; color:grey; padding-top:24px;\">"
        "<p>No Nexus description stored for this mod. Press "
        "<b>Refresh</b> to fetch it live.</p></div>"));
    return;
  }
  // BBCode parse + QTextBrowser layout is moved off the UI thread for
  // descriptions >= 1 KB (most Nexus descriptions). The async helper
  // posts the wrapped HTML back via QMetaObject::invokeMethod and uses
  // description_generation_ to discard stale results when the user
  // clicks rapidly through the mod list.
  ++description_generation_;
  set_bbcode_html_async(description_, stored, &description_generation_);
}

void NexusSourcePanel::on_refresh() {
  if (!data_.fetch_nexus_info)
    return;
  if (mod_id_ == nullptr)
    return;
  const QString id = mod_id_->text().trimmed();
  if (id.isEmpty() || id.toInt() <= 0)
    return;

  ++refresh_generation_;
  if (fetch_in_flight_) {
    refresh_pending_ = true;
    return;
  }
  launch_fetch();
}

void NexusSourcePanel::launch_fetch() {
  fetch_in_flight_ = true;
  refresh_pending_ = false;
  refresh_mod_id_ = data_.id;
  const quint64 gen = refresh_generation_;
  auto fetch = data_.fetch_nexus_info;

  if (refresh_ != nullptr) {
    refresh_->setEnabled(false);
    refresh_->setText(tr("Fetching…"));
  }

  if (source_fetch_thread_ == nullptr) {
    source_fetch_thread_ = new SourceFetchThread(this);
    connect(source_fetch_thread_->worker(), &SourceFetchWorker::finished, this,
            &NexusSourcePanel::on_fetch_finished);
  }
  source_fetch_thread_->start(std::move(fetch), gen);
}

void NexusSourcePanel::on_fetch_finished(engine::ModInfoResult result,
                                         quint64 generation) {
  fetch_in_flight_ = false;
  if (refresh_ != nullptr) {
    refresh_->setEnabled(true);
    refresh_->setText(tr("Refresh"));
  }

  const bool stale =
      generation != refresh_generation_ || refresh_mod_id_ != data_.id;
  const bool relaunch = refresh_pending_;
  refresh_pending_ = false;

  if (!stale)
    apply_fetch_result(result);
  if (relaunch)
    launch_fetch();
}

void NexusSourcePanel::apply_fetch_result(const engine::ModInfoResult &result) {
  if (!result.available) {
    render_description();
    return;
  }

  auto meta = data_.load_meta();
  const QString name = QString::fromStdString(result.name);
  if (!name.isEmpty())
    meta.set("General", "name", name.toStdString());
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
  data_.save_meta(meta);

  // Re-read from updated meta
  populate();
}

void NexusSourcePanel::on_visit() {
  if (mod_id_ == nullptr)
    return;
  const QString id = mod_id_->text().trimmed();
  if (id.isEmpty() || id.toInt() <= 0)
    return;
  if (!data_.open_url)
    return;
  if (data_.nexus_domain.isEmpty())
    return;
  data_.open_url(QStringLiteral("https://www.nexusmods.com/%1/mods/%2")
                     .arg(data_.nexus_domain, id));
}

void NexusSourcePanel::on_visit_custom() {
  if (custom_url_ == nullptr)
    return;
  const QString url = custom_url_->text().trimmed();
  if (url.isEmpty())
    return;
  if (!data_.open_url)
    return;
  data_.open_url(url);
}

void NexusSourcePanel::on_custom_url_toggled() {
  if (loading_)
    return;
  custom_url_->setEnabled(custom_url_toggle_->isChecked());
  visit_custom_->setEnabled(custom_url_toggle_->isChecked() &&
                            !custom_url_->text().isEmpty());
  set_meta_value("General", "hasCustomURL",
                 custom_url_toggle_->isChecked() ? QStringLiteral("true")
                                                 : QStringLiteral("false"));
}

void NexusSourcePanel::persist_fields() {
  if (loading_)
    return;
  if (mod_id_ == nullptr)
    return;
  // Workspace-rvld: do not fabricate a [Nexusmods] section for mods that
  // are NOT actually from Nexus. The panel can render for a non-Nexus mod
  // (LoversLab / Steam / manual) when SourceTab unions present meta
  // sources with the game hook's supported_sources so provenance is
  // never hidden - in that case the fields are blank and any accidental
  // edit here must NOT be persisted as Nexus meta. Only allow the write
  // when this mod is already marked as Nexus-sourced OR already has a
  // [Nexusmods] section (legacy data).
  const QString modid_text = mod_id_->text().trimmed();
  const QString existing_modid = meta_value("Nexusmods", "modid");
  const bool is_nexus_mod =
      (data_.source_type == QLatin1String("nexus")) ||
      !existing_modid.isEmpty();
  if (is_nexus_mod) {
    set_meta_value("Nexusmods", "modid", modid_text);
  }
  // version is a [General] key shared by all sources - always persist so
  // Steam / LoversLab users editing the version field still see their
  // change land.
  set_meta_value("General", "version", version_->text().trimmed());
  // category belongs to [Nexusmods] - only persist when this is genuinely
  // a Nexus mod (otherwise we pollute the wrong namespace).
  if (is_nexus_mod) {
    set_meta_value("Nexusmods", "nexuscategory", category_->text().trimmed());
  }
}

void NexusSourcePanel::persist_custom_url() {
  if (loading_)
    return;
  if (custom_url_ == nullptr)
    return;
  set_meta_value("General", "url", custom_url_->text().trimmed());
  visit_custom_->setEnabled(custom_url_toggle_->isChecked() &&
                            !custom_url_->text().isEmpty());
}

} // namespace ui
