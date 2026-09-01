#include "ui/modinfo/source_panels/loverslab_source_panel.h"

#include "ui/modinfo/bbcode.h"
#include "ui/modinfo/description_browser.h"
#include "ui/modinfo/loverslab_fetch_worker.h"

#include <QDate>
#include <QDateTime>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QScopedValueRollback>
#include <QTimeZone>
#include <QVBoxLayout>

namespace ui {

namespace {

void set_description_html(DescriptionBrowser *browser, const QString &desc) {
  if (browser == nullptr)
    return;
  // Drop any in-flight image fetches / cached resources from the previous
  // render so we never display a picture from the prior mod here. Same
  // sanitization pipeline as Nexus / Steam descriptions: bbcode_to_html
  // strips javascript:/data: URLs and CSS meta-chars, then we wrap in
  // pre-wrap HTML so raw \n newlines survive.
  browser->clear_image_cache();
  if (desc.isEmpty()) {
    browser->setHtml(QStringLiteral(
        "<div style=\"text-align:center; color:grey; padding-top:24px;\">"
        "<p>No LoversLab description stored for this mod. Press "
        "<b>Refresh</b> to fetch it live.</p></div>"));
    return;
  }
  browser->setHtml(
      QStringLiteral(
          "<html><body style=\"font-family:sans-serif; white-space:pre-wrap;\">"
          "%1</body></html>")
          .arg(bbcode_to_html(desc)));
}

// Parse the page's dateModified into a QDate. Accepts the two shapes the
// schema.org dateModified / og:updated_time values arrive in:
//   - "2025-06-05"                          (date only)
//   - "2025-06-05T12:34:56" / "+02:00"      (with time, possibly with TZ)
// Anything else returns an invalid date and the out-of-date badge stays
// hidden (Unknown state).
QDate parse_date_modified(const QString &raw) {
  if (raw.isEmpty())
    return {};
  QDateTime dt = QDateTime::fromString(raw, Qt::ISODate);
  if (!dt.isValid())
    dt = QDateTime::fromString(raw.left(10), QStringLiteral("yyyy-MM-dd"));
  if (!dt.isValid())
    return {};
  return dt.date();
}

// Inverse: epoch seconds -> QDate (UTC). The page's dateModified is
// always normalized to UTC by the page; the install timestamp is the
// filesystem clock - on Linux btime is wall clock UTC. We compare at day
// granularity so timezone offsets cannot push a same-day install into
// the "page is newer" bucket. Uses QDateTime instead of gmtime_r so the
// build is portable to MSVC (which lacks gmtime_r) and to keep with the
// rest of the Qt-using panel. Uses QTimeZone::UTC rather than the
// deprecated Qt::TimeSpec overload.
QDate epoch_to_date(qint64 ts) {
  if (ts <= 0)
    return {};
  return QDateTime::fromSecsSinceEpoch(ts, QTimeZone::UTC).date();
}

} // namespace

LoversLabSourcePanel::LoversLabSourcePanel(const ModInfoData &data,
                                           QWidget *parent)
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
  layout->addLayout(form);

  auto *buttons = new QHBoxLayout();
  refresh_ = new QPushButton(tr("Refresh"), this);
  visit_ = new QPushButton(tr("Visit on LoversLab"), this);
  buttons->addWidget(refresh_);
  buttons->addWidget(visit_);
  buttons->addStretch(1);
  layout->addLayout(buttons);

  // Out-of-date badge - hidden until populate() decides whether the
  // installed copy is out of date. Red text, full row, sits between the
  // buttons and the custom-URL row so it is the most prominent status
  // signal in the panel.
  out_of_date_label_ = new QLabel(this);
  out_of_date_label_->setTextFormat(Qt::PlainText);
  out_of_date_label_->setWordWrap(true);
  out_of_date_label_->setVisible(false);
  layout->addWidget(out_of_date_label_);

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
          &LoversLabSourcePanel::persist_fields);
  connect(version_, &QLineEdit::editingFinished, this,
          &LoversLabSourcePanel::persist_fields);
  connect(category_, &QLineEdit::editingFinished, this,
          &LoversLabSourcePanel::persist_fields);
  connect(custom_url_, &QLineEdit::editingFinished, this,
          &LoversLabSourcePanel::persist_custom_url);
  connect(custom_url_toggle_, &QCheckBox::toggled, this,
          &LoversLabSourcePanel::on_custom_url_toggled);
  connect(refresh_, &QPushButton::clicked, this,
          &LoversLabSourcePanel::on_refresh);
  connect(visit_, &QPushButton::clicked, this, &LoversLabSourcePanel::on_visit);
  connect(visit_custom_, &QPushButton::clicked, this,
          &LoversLabSourcePanel::on_visit_custom);

  populate();
}

LoversLabSourcePanel::~LoversLabSourcePanel() = default;

void LoversLabSourcePanel::populate() {
  QScopedValueRollback<bool> guard(loading_, true);

  // Mod ID: [LoversLab]fileid fallback [GameModManager]source_id
  // fallback data_.source_id. Same precedence as Steam's workshop_id.
  QString fid = meta_value("LoversLab", "fileid");
  if (fid.isEmpty())
    fid = meta_value("GameModManager", "source_id");
  if (fid.isEmpty())
    fid = data_.source_id;
  mod_id_->setText(fid);

  // Lock Mod ID when the mod is confirmed LoversLab-sourced with a
  // numeric id - matches Steam's behavior so the panel does not let the
  // user overwrite a real source id with garbage.
  const bool is_confirmed_ll =
      (data_.source_type == QLatin1String("loverslab") && !fid.isEmpty() &&
       fid.toLongLong() > 0);
  mod_id_->setReadOnly(is_confirmed_ll);

  version_->setText(meta_value("General", "version"));
  if (version_->text().isEmpty())
    version_->setText(data_.version);

  category_->setText(meta_value("LoversLab", "category"));

  custom_url_toggle_->setChecked(meta_value("General", "hasCustomURL") ==
                                 QStringLiteral("true"));
  custom_url_->setText(meta_value("General", "url"));
  custom_url_->setEnabled(custom_url_toggle_->isChecked());
  visit_custom_->setEnabled(custom_url_toggle_->isChecked() &&
                            !custom_url_->text().isEmpty());

  update_version_color();
  update_out_of_date_label();
  render_description();
}

bool LoversLabSourcePanel::has_data() const {
  // The panel truly has LoversLab data only when the mod is actually
  // LoversLab-sourced (data_.source_type == "loverslab") OR a [LoversLab]
  // section already exists in the sidecar meta (legacy data, future
  // refresh, manual user edit). Pure version-only mods (the default "1.0"
  // stamped on every install) are NOT LoversLab has_data - that would be
  // the same Workspace-rvld bug NexusSourcePanel guards against.
  if (data_.source_type == QLatin1String("loverslab") &&
      !data_.source_id.isEmpty())
    return true;
  if (data_.load_meta) {
    auto meta = data_.load_meta();
    if (meta.has_section("LoversLab"))
      return true;
  }
  // Fallback: any persistent LoversLab key means the user has touched
  // the panel for this mod (or a previous Refresh landed).
  return !meta_value("LoversLab", "fileid").isEmpty() ||
         !meta_value("LoversLab", "category").isEmpty() ||
         !meta_value("LoversLab", "description").isEmpty();
}

void LoversLabSourcePanel::save_state() {
  persist_fields();
  persist_custom_url();
}

LoversLabSourcePanel::DateCompare LoversLabSourcePanel::compare_dates() const {
  const QDate page_date =
      parse_date_modified(meta_value("LoversLab", "date_modified"));
  const QDate install_date = epoch_to_date(data_.installation_ts);

  // Per the analysis: mods downloaded before this feature landed
  // (date_modified absent) and manual mods (no LoversLab section) must
  // report Unknown, NOT out-of-date.
  if (!page_date.isValid() || !install_date.isValid())
    return DateCompare::Unknown;
  if (page_date == install_date)
    return DateCompare::SameDay;
  return page_date > install_date ? DateCompare::PageNewer
                                  : DateCompare::PageOlder;
}

void LoversLabSourcePanel::update_out_of_date_label() {
  if (out_of_date_label_ == nullptr)
    return;
  switch (compare_dates()) {
  case DateCompare::PageNewer: {
    const QString page_date = meta_value("LoversLab", "date_modified").left(10);
    const QString installed = epoch_to_date(data_.installation_ts)
                                  .toString(QStringLiteral("yyyy-MM-dd"));
    // Red, bold. Tooltip explains the comparison for curious users.
    out_of_date_label_->setText(
        tr("This mod is out of date - the page was updated on %1 but the "
           "installed copy is from %2.")
            .arg(page_date, installed));
    out_of_date_label_->setStyleSheet(
        QStringLiteral("color: red; font-weight: bold;"));
    out_of_date_label_->setToolTip(
        tr("The page's dateModified is later than this mod's install time. "
           "Press Refresh to fetch the new version's metadata."));
    out_of_date_label_->setVisible(true);
    break;
  }
  case DateCompare::PageOlder:
  case DateCompare::SameDay:
    // Installed copy is at least as recent as the page advertises -
    // nothing to flag. Older pages are possible (page lags the install)
    // and we don't want a "you're ahead" green badge to confuse users.
    out_of_date_label_->setVisible(false);
    out_of_date_label_->setText({});
    out_of_date_label_->setStyleSheet({});
    out_of_date_label_->setToolTip({});
    break;
  case DateCompare::Unknown:
    // No date_modified stored (pre-feature mod) or no install_ts (manual
    // / separator) -> we cannot compare, so we cannot flag. Hide.
    out_of_date_label_->setVisible(false);
    out_of_date_label_->setText({});
    out_of_date_label_->setStyleSheet({});
    out_of_date_label_->setToolTip({});
    break;
  }
}

void LoversLabSourcePanel::update_version_color() {
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

void LoversLabSourcePanel::render_description() {
  if (description_ == nullptr)
    return;
  const QString stored = meta_value("LoversLab", "description");
  set_description_html(description_, stored);
}

void LoversLabSourcePanel::on_refresh() {
  if (!data_.fetch_loverslab_info)
    return;
  if (mod_id_ == nullptr)
    return;
  const QString fid = mod_id_->text().trimmed();
  if (fid.isEmpty() || fid.toLongLong() <= 0)
    return;

  ++refresh_generation_;
  if (fetch_in_flight_) {
    refresh_pending_ = true;
    return;
  }
  launch_fetch();
}

void LoversLabSourcePanel::launch_fetch() {
  fetch_in_flight_ = true;
  refresh_pending_ = false;
  refresh_mod_id_ = data_.id;
  const quint64 gen = refresh_generation_;
  auto fetch = data_.fetch_loverslab_info;

  if (refresh_ != nullptr) {
    refresh_->setEnabled(false);
    refresh_->setText(tr("Fetching…"));
  }

  if (source_fetch_thread_ == nullptr) {
    source_fetch_thread_ = new LoversLabFetchThread(this);
    connect(source_fetch_thread_->worker(), &LoversLabFetchWorker::finished,
            this, &LoversLabSourcePanel::on_fetch_finished);
  }
  source_fetch_thread_->start(std::move(fetch), gen);
}

void LoversLabSourcePanel::on_fetch_finished(
    engine::LoversLabModInfoResult result, quint64 generation) {
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

void LoversLabSourcePanel::apply_fetch_result(
    const engine::LoversLabModInfoResult &result) {
  if (!result.available) {
    render_description();
    update_out_of_date_label();
    return;
  }

  auto meta = data_.load_meta();
  // fileid: re-write from the panel's edit field so a successful Refresh
  // confirms the id and a user-cleared field stays cleared (matching
  // Nexus's behavior of writing the panel's modid back to [Nexusmods]).
  if (mod_id_ != nullptr) {
    const QString fid = mod_id_->text().trimmed();
    if (!fid.isEmpty())
      meta.set("LoversLab", "fileid", fid.toStdString());
  }
  // General fields shared by every source - mirrors Nexus writeback so
  // the Mod Info overview / list view sees the same version string.
  const QString name = QString::fromStdString(result.name);
  if (!name.isEmpty())
    meta.set("General", "name", name.toStdString());
  if (!result.version.empty())
    meta.set("General", "version",
             QString::fromStdString(result.version).toStdString());
  // LoversLab has no "newest_version" concept (single live revision per
  // file), but keep the key parity so the version-color logic in
  // update_version_color() has something to compare against if the user
  // manually fills it.
  if (!result.category.empty())
    meta.set("LoversLab", "category",
             QString::fromStdString(result.category).toStdString());
  if (!result.description.empty())
    meta.set("LoversLab", "description",
             QString::fromStdString(result.description).toStdString());
  if (!result.author.empty())
    meta.set("LoversLab", "author",
             QString::fromStdString(result.author).toStdString());
  if (!result.date_modified.empty())
    meta.set("LoversLab", "date_modified",
             QString::fromStdString(result.date_modified).toStdString());
  if (!result.page_url.empty()) {
    meta.set("LoversLab", "page_url",
             QString::fromStdString(result.page_url).toStdString());
    // Mirror into ModMeta::source_page_url()'s lookup by overwriting the
    // [LoversLab] page_url, which is exactly what that accessor reads.
  }
  data_.save_meta(meta);

  populate();
}

void LoversLabSourcePanel::on_visit() {
  const QString fid = mod_id_ ? mod_id_->text().trimmed() : QString();
  if (fid.isEmpty() || fid.toLongLong() <= 0)
    return;
  if (!data_.open_url)
    return;
  // Prefer the persisted page_url (carries the slug); fall back to the
  // canonical bare-id URL.
  QString url = meta_value("LoversLab", "page_url");
  if (url.isEmpty())
    url = QStringLiteral("https://www.loverslab.com/files/file/") + fid +
          QStringLiteral("/");
  data_.open_url(url);
}

void LoversLabSourcePanel::on_visit_custom() {
  if (custom_url_ == nullptr)
    return;
  const QString url = custom_url_->text().trimmed();
  if (url.isEmpty())
    return;
  if (!data_.open_url)
    return;
  data_.open_url(url);
}

void LoversLabSourcePanel::on_custom_url_toggled() {
  if (loading_)
    return;
  custom_url_->setEnabled(custom_url_toggle_->isChecked());
  visit_custom_->setEnabled(custom_url_toggle_->isChecked() &&
                            !custom_url_->text().isEmpty());
  set_meta_value("General", "hasCustomURL",
                 custom_url_toggle_->isChecked() ? QStringLiteral("true")
                                                 : QStringLiteral("false"));
}

void LoversLabSourcePanel::persist_fields() {
  if (loading_)
    return;
  if (mod_id_ == nullptr)
    return;
  const QString fid = mod_id_->text().trimmed();
  const QString existing_fid = meta_value("LoversLab", "fileid");
  const bool is_loverslab_mod =
      (data_.source_type == QLatin1String("loverslab")) ||
      !existing_fid.isEmpty();
  if (is_loverslab_mod) {
    set_meta_value("LoversLab", "fileid", fid);
  }
  // version is a [General] key shared by all sources - always persist so
  // Steam / Nexus users editing the version field still see their change
  // land.
  set_meta_value("General", "version", version_->text().trimmed());
  // category belongs to [LoversLab] - only persist when this is genuinely
  // a LoversLab mod (otherwise we pollute the wrong namespace).
  if (is_loverslab_mod) {
    set_meta_value("LoversLab", "category", category_->text().trimmed());
  }
}

void LoversLabSourcePanel::persist_custom_url() {
  if (loading_)
    return;
  if (custom_url_ == nullptr)
    return;
  set_meta_value("General", "url", custom_url_->text().trimmed());
  visit_custom_->setEnabled(custom_url_toggle_->isChecked() &&
                            !custom_url_->text().isEmpty());
}

} // namespace ui