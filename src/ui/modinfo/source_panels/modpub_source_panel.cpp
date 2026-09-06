#include "ui/modinfo/source_panels/modpub_source_panel.h"

#include "ui/modinfo/bbcode.h"
#include "ui/modinfo/description_browser.h"
#include "ui/modinfo/modpub_fetch_worker.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QScopedValueRollback>
#include <QVBoxLayout>

#include <atomic>

namespace ui {

namespace {

void set_description_html(DescriptionBrowser *browser, const QString &desc,
                          std::atomic<unsigned> *gen) {
  if (browser == nullptr)
    return;
  // Drop any in-flight image fetches / cached resources from the previous
  // render so we never display a picture from the prior mod here. Same
  // sanitization pipeline as the other source panels: bbcode_to_html
  // strips javascript:/data: URLs and CSS meta-chars, then we wrap in
  // pre-wrap HTML so raw \n newlines survive.
  browser->clear_image_cache();
  if (desc.isEmpty()) {
    browser->setHtml(QStringLiteral(
        "<div style=\"text-align:center; color:grey; padding-top:24px;\">"
        "<p>No mod.pub description stored for this mod. Press "
        "<b>Refresh</b> to fetch it live.</p></div>"));
    return;
  }
  // BBCode parse + QTextBrowser layout moves off the UI thread for
  // descriptions >= 1 KB. The async helper uses `gen` to drop stale
  // results when the user clicks rapidly through the mod list.
  if (gen != nullptr)
    ++*gen;
  set_bbcode_html_async(browser, desc, gen);
}

} // namespace

ModPubSourcePanel::ModPubSourcePanel(const ModInfoData &data, QWidget *parent)
    : SourceInfoPanel(data, parent) {
  auto *layout = new QVBoxLayout(this);

  auto *form = new QFormLayout();
  mod_id_ = new QLineEdit(this);
  mod_id_->setPlaceholderText(QStringLiteral("0"));
  form->addRow(tr("Mod ID:"), mod_id_);

  version_ = new QLineEdit(this);
  form->addRow(tr("Version:"), version_);

  category_ = new QLineEdit(this);
  category_->setPlaceholderText(QStringLiteral("0"));
  form->addRow(tr("Category:"), category_);

  author_ = new QLineEdit(this);
  author_->setReadOnly(true);
  form->addRow(tr("Author:"), author_);

  page_url_ = new QLineEdit(this);
  page_url_->setPlaceholderText(
      QStringLiteral("https://mod.pub/<game>/<id>-<slug>"));
  form->addRow(tr("Page URL:"), page_url_);
  layout->addLayout(form);

  auto *buttons = new QHBoxLayout();
  refresh_ = new QPushButton(tr("Refresh"), this);
  visit_ = new QPushButton(tr("Visit on ModPub"), this);
  buttons->addWidget(refresh_);
  buttons->addWidget(visit_);
  buttons->addStretch(1);
  layout->addLayout(buttons);

  description_ = new DescriptionBrowser(this);
  description_->setOpenExternalLinks(true);
  layout->addWidget(description_, 1);

  connect(mod_id_, &QLineEdit::editingFinished, this,
          &ModPubSourcePanel::persist_fields);
  connect(version_, &QLineEdit::editingFinished, this,
          &ModPubSourcePanel::persist_fields);
  connect(category_, &QLineEdit::editingFinished, this,
          &ModPubSourcePanel::persist_fields);
  connect(page_url_, &QLineEdit::editingFinished, this,
          &ModPubSourcePanel::persist_fields);
  connect(refresh_, &QPushButton::clicked, this,
          &ModPubSourcePanel::on_refresh);
  connect(visit_, &QPushButton::clicked, this, &ModPubSourcePanel::on_visit);

  populate();
}

ModPubSourcePanel::~ModPubSourcePanel() = default;

void ModPubSourcePanel::populate() {
  QScopedValueRollback<bool> guard(loading_, true);

  // Mod ID: [ModPub]mod_id fallback [GameModManager]source_id fallback
  // data_.source_id. Same precedence as LoversLab's fileid lookup.
  QString mid = meta_value("ModPub", "mod_id");
  if (mid.isEmpty())
    mid = meta_value("GameModManager", "source_id");
  if (mid.isEmpty())
    mid = data_.source_id;
  mod_id_->setText(mid);

  // Lock the Mod ID when the mod is confirmed ModPub-sourced with a
  // numeric id - matches LoversLab/Steam behavior so the panel does not
  // let the user overwrite a real source id with garbage.
  const bool is_confirmed_mp =
      (data_.source_type == QLatin1String("modpub") && !mid.isEmpty() &&
       mid.toLongLong() > 0);
  mod_id_->setReadOnly(is_confirmed_mp);

  version_->setText(meta_value("General", "version"));
  if (version_->text().isEmpty())
    version_->setText(data_.version);

  category_->setText(meta_value("ModPub", "category"));
  author_->setText(meta_value("ModPub", "author"));
  page_url_->setText(meta_value("ModPub", "page_url"));

  render_description();
}

bool ModPubSourcePanel::has_data() const {
  // The panel truly has ModPub data only when the mod is actually
  // ModPub-sourced (data_.source_type == "modpub") OR a [ModPub]
  // section already exists in the sidecar meta (legacy data, future
  // refresh, manual user edit). Pure version-only mods (the default
  // "1.0" stamped on every install) are NOT ModPub has_data.
  if (data_.source_type == QLatin1String("modpub") &&
      !data_.source_id.isEmpty())
    return true;
  if (data_.load_meta) {
    auto meta = data_.load_meta();
    if (meta.has_section("ModPub"))
      return true;
  }
  // Fallback: any persistent ModPub key means the user has touched the
  // panel for this mod (or a previous Refresh landed).
  return !meta_value("ModPub", "mod_id").isEmpty() ||
         !meta_value("ModPub", "category").isEmpty() ||
         !meta_value("ModPub", "description").isEmpty();
}

void ModPubSourcePanel::save_state() { persist_fields(); }

void ModPubSourcePanel::render_description() {
  if (description_ == nullptr)
    return;
  const QString stored = meta_value("ModPub", "description");
  set_description_html(description_, stored, &description_generation_);
}

void ModPubSourcePanel::on_refresh() {
  if (!data_.fetch_modpub_info)
    return;
  if (mod_id_ == nullptr)
    return;
  // ModPub fetches a page; we need either the page URL or the bare mod
  // id to call the provider. The mod_id field is the id; page_url is
  // captured by the fetcher lambda in mod_list_controller. We only gate
  // here on the id being parseable (the fetcher will pick the better
  // of the two at the call site).
  const QString mid = mod_id_->text().trimmed();
  if (mid.isEmpty() || mid.toLongLong() <= 0)
    return;

  ++refresh_generation_;
  if (fetch_in_flight_) {
    refresh_pending_ = true;
    return;
  }
  launch_fetch();
}

void ModPubSourcePanel::launch_fetch() {
  fetch_in_flight_ = true;
  refresh_pending_ = false;
  refresh_mod_id_ = data_.id;
  const quint64 gen = refresh_generation_;
  auto fetch = data_.fetch_modpub_info;

  if (refresh_ != nullptr) {
    refresh_->setEnabled(false);
    refresh_->setText(tr("Fetching…"));
  }

  if (source_fetch_thread_ == nullptr) {
    source_fetch_thread_ = new ModPubFetchThread(this);
    connect(source_fetch_thread_->worker(), &ModPubFetchWorker::finished,
            this, &ModPubSourcePanel::on_fetch_finished);
  }
  source_fetch_thread_->start(std::move(fetch), gen);
}

void ModPubSourcePanel::on_fetch_finished(
    engine::ModPubModInfoResult result, quint64 generation) {
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

void ModPubSourcePanel::apply_fetch_result(
    const engine::ModPubModInfoResult &result) {
  if (!result.available) {
    render_description();
    return;
  }

  auto meta = data_.load_meta();
  // mod_id: re-write from the panel's edit field so a successful Refresh
  // confirms the id and a user-cleared field stays cleared (matches
  // LoversLab's behavior of writing the panel's id back to the
  // per-source section).
  if (mod_id_ != nullptr) {
    const QString mid = mod_id_->text().trimmed();
    if (!mid.isEmpty())
      meta.set("ModPub", "mod_id", mid.toStdString());
  }
  // General fields shared by every source - mirrors LoversLab writeback
  // so the Mod Info overview / list view sees the same version string.
  const QString name = QString::fromStdString(result.name);
  if (!name.isEmpty())
    meta.set("General", "name", name.toStdString());
  if (!result.version.empty())
    meta.set("General", "version",
             QString::fromStdString(result.version).toStdString());
  // ModPub's applicationCategory is the boilerplate "GameMod" string;
  // the parser overwrites it with the aside tag (the human-readable
  // category like "User interface"). We persist both to [ModPub] so
  // future schema-only consumers (which may want the original
  // applicationCategory) get the raw value, and the panel reads the
  // more useful tag by default.
  if (!result.category.empty())
    meta.set("ModPub", "category",
             QString::fromStdString(result.category).toStdString());
  if (!result.description.empty())
    meta.set("ModPub", "description",
             QString::fromStdString(result.description).toStdString());
  if (!result.author.empty())
    meta.set("ModPub", "author",
             QString::fromStdString(result.author).toStdString());
  if (!result.date_modified.empty())
    meta.set("ModPub", "date_modified",
             QString::fromStdString(result.date_modified).toStdString());
  if (!result.page_url.empty())
    meta.set("ModPub", "page_url",
             QString::fromStdString(result.page_url).toStdString());
  if (!result.game_slug.empty())
    meta.set("ModPub", "game_slug",
             QString::fromStdString(result.game_slug).toStdString());
  data_.save_meta(meta);

  populate();
}

void ModPubSourcePanel::on_visit() {
  // Prefer the persisted page URL (carries the game-slug and the
  // slug-suffix). Fall back to the canonical https://mod.pub/mods/<id>/
  // form when the panel has only the id (e.g. the URL was never set).
  QString url = page_url_ ? page_url_->text().trimmed() : QString();
  if (url.isEmpty()) {
    const QString mid = mod_id_ ? mod_id_->text().trimmed() : QString();
    if (mid.isEmpty() || mid.toLongLong() <= 0)
      return;
    url = QStringLiteral("https://mod.pub/mods/") + mid + QStringLiteral("/");
  }
  if (!data_.open_url)
    return;
  data_.open_url(url);
}

void ModPubSourcePanel::persist_fields() {
  if (loading_)
    return;
  if (mod_id_ == nullptr)
    return;
  const QString mid = mod_id_->text().trimmed();
  const QString existing_mid = meta_value("ModPub", "mod_id");
  const bool is_modpub_mod =
      (data_.source_type == QLatin1String("modpub")) ||
      !existing_mid.isEmpty();
  if (is_modpub_mod) {
    set_meta_value("ModPub", "mod_id", mid);
  }
  // version is a [General] key shared by all sources - always persist so
  // Steam / Nexus users editing the version field still see their
  // change land.
  set_meta_value("General", "version", version_->text().trimmed());
  // The remaining [ModPub] keys (category, page_url) only persist when
  // this is genuinely a ModPub mod (otherwise we pollute the wrong
  // namespace). Author is set by the fetcher and is read-only here.
  if (is_modpub_mod) {
    set_meta_value("ModPub", "category", category_->text().trimmed());
    set_meta_value("ModPub", "page_url", page_url_->text().trimmed());
  }
}

} // namespace ui
