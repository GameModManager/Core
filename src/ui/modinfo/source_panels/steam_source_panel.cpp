#include "ui/modinfo/source_panels/steam_source_panel.h"

#include "engine/mod/meta/xml_util.h"
#include "ui/modinfo/bbcode.h"
#include "ui/modinfo/description_browser.h"

#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QScopedValueRollback>
#include <QVBoxLayout>

#include <atomic>

namespace ui {

namespace {

QString xml_find_tag_text(const QString &xml, const QString &tag) {
  const std::string xml_s = xml.toStdString();
  const std::string tag_s = tag.toStdString();
  const std::string result = engine::xml_find_tag(xml_s, tag_s);
  return QString::fromStdString(result);
}

QString load_metadata_content(const ModInfoData &data) {
  if (data.mod_dir.path().isEmpty())
    return {};
  const QString file_name = data.metadata_file.isEmpty()
                                ? QStringLiteral("metadata.xml")
                                : data.metadata_file;
  const QString xml_path = data.mod_dir.filePath(file_name);
  QFile f(xml_path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return {};
  const QString content = QString::fromUtf8(f.readAll());
  return content;
}

void set_description_html(DescriptionBrowser *browser, const QString &desc,
                          std::atomic<unsigned> *gen) {
  if (browser == nullptr)
    return;
  // Drop any in-flight image fetches / cached resources from the previous
  // render so we never display a picture from the prior mod here.
  browser->clear_image_cache();
  if (desc.isEmpty()) {
    browser->setHtml(QStringLiteral(
        "<div style=\"text-align:center; color:grey; padding-top:24px;\">"
        "<p>No description stored for this mod. Press "
        "<b>Refresh</b> to re-read metadata.</p></div>"));
    return;
  }
  // Steam Workshop descriptions are BBCode (b/i/u/url/img/quote/etc), same
  // dialect the Nexus source panel parses. The old plain-text-escape path
  // hid all of that from the user; libcbb now renders it. The async
  // helper moves the parse + QTextBrowser layout off the UI thread for
  // descriptions >= 1 KB and uses `gen` to drop stale results when the
  // user clicks rapidly through the mod list.
  if (gen != nullptr)
    ++*gen;
  set_bbcode_html_async(browser, desc, gen);
}

} // namespace

SteamSourcePanel::SteamSourcePanel(const ModInfoData &data, QWidget *parent)
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
  visit_ = new QPushButton(tr("Visit on Workshop"), this);
  buttons->addWidget(refresh_);
  buttons->addWidget(visit_);
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
          &SteamSourcePanel::persist_fields);
  connect(version_, &QLineEdit::editingFinished, this,
          &SteamSourcePanel::persist_fields);
  connect(category_, &QLineEdit::editingFinished, this,
          &SteamSourcePanel::persist_fields);
  connect(custom_url_, &QLineEdit::editingFinished, this,
          &SteamSourcePanel::persist_custom_url);
  connect(custom_url_toggle_, &QCheckBox::toggled, this,
          &SteamSourcePanel::on_custom_url_toggled);
  connect(refresh_, &QPushButton::clicked, this, &SteamSourcePanel::on_refresh);
  connect(visit_, &QPushButton::clicked, this, &SteamSourcePanel::on_visit);
  connect(visit_custom_, &QPushButton::clicked, this,
          &SteamSourcePanel::on_visit_custom);

  populate();
}

void SteamSourcePanel::populate() {
  QScopedValueRollback<bool> guard(loading_, true);

  // Mod ID: [SteamWorkshop]workshop_id fallback [GameModManager]source_id
  // fallback current().source_id
  QString wid = meta_value("SteamWorkshop", "workshop_id");
  if (wid.isEmpty())
    wid = meta_value("GameModManager", "source_id");
  if (wid.isEmpty())
    wid = data_.source_id;
  mod_id_->setText(wid);

  // Lock Mod ID when source is confirmed Steam (numeric id and source_type ==
  // steam)
  const bool is_confirmed_steam =
      (data_.source_type == QLatin1String("steam") && !wid.isEmpty() &&
       wid.toLongLong() > 0);
  mod_id_->setReadOnly(is_confirmed_steam);

  // Version: [General]version fallback current.version fallback metadata.xml
  // <version>
  QString ver = meta_value("General", "version");
  if (ver.isEmpty())
    ver = data_.version;
  if (ver.isEmpty())
    ver = read_metadata_tag(QStringLiteral("version"));
  version_->setText(ver);

  // Category: [General]category (CSV primary first)
  category_->setText(meta_value("General", "category"));

  custom_url_toggle_->setChecked(meta_value("General", "hasCustomURL") ==
                                 QStringLiteral("true"));
  custom_url_->setText(meta_value("General", "url"));
  custom_url_->setEnabled(custom_url_toggle_->isChecked());
  visit_custom_->setEnabled(custom_url_toggle_->isChecked() &&
                            !custom_url_->text().isEmpty());

  update_version_color();
  render_description();
}

bool SteamSourcePanel::has_data() const {
  // Steam panel has data when there is a workshop id or version
  // Keep in sync with populate() wid lookup.
  QString wid = meta_value("SteamWorkshop", "workshop_id");
  if (wid.isEmpty())
    wid = meta_value("GameModManager", "source_id");
  if (wid.isEmpty())
    wid = data_.source_id;
  return !wid.isEmpty() || !meta_value("General", "version").isEmpty() ||
         !data_.version.isEmpty();
}

void SteamSourcePanel::save_state() {
  persist_fields();
  persist_custom_url();
}

QString SteamSourcePanel::read_metadata_tag(const QString &tag) const {
  const QString content = load_metadata_content(data_);
  if (content.isEmpty())
    return {};
  return xml_find_tag_text(content, tag);
}

void SteamSourcePanel::update_version_color() {
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

void SteamSourcePanel::render_description() {
  if (description_ == nullptr)
    return;

  // Priority: metadata file <description> else [SteamWorkshop]description else
  // placeholder
  QString desc = read_metadata_tag(QStringLiteral("description"));
  if (desc.isEmpty())
    desc = meta_value("SteamWorkshop", "description");

  set_description_html(description_, desc, &description_generation_);
}

void SteamSourcePanel::on_refresh() {
  // Synchronous local re-read of the game's metadata file; update UI fields.
  // Reads the file once and reuses the content for both version and description
  // to avoid double I/O.
  QScopedValueRollback<bool> guard(loading_, true);

  const QString content = load_metadata_content(data_);
  const QString xml_version =
      content.isEmpty() ? QString{}
                        : xml_find_tag_text(content, QStringLiteral("version"));
  const QString xml_desc =
      content.isEmpty()
          ? QString{}
          : xml_find_tag_text(content, QStringLiteral("description"));

  // Re-populate version if file has a value
  if (!xml_version.isEmpty()) {
    version_->setText(xml_version);
  } else {
    // Fall back to sidecar / current
    QString ver = meta_value("General", "version");
    if (ver.isEmpty())
      ver = data_.version;
    version_->setText(ver);
  }

  // Category stays as sidecar value (tags mapping not in XML)
  category_->setText(meta_value("General", "category"));

  // Reuse already-read xml_desc instead of re-reading the file in
  // render_description()
  QString desc = xml_desc;
  if (desc.isEmpty())
    desc = meta_value("SteamWorkshop", "description");
  set_description_html(description_, desc, &description_generation_);
  update_version_color();
}

void SteamSourcePanel::on_visit() {
  if (mod_id_ == nullptr)
    return;
  const QString id = mod_id_->text().trimmed();
  if (id.isEmpty() || id.toLongLong() <= 0)
    return;
  if (!data_.open_url)
    return;
  data_.open_url(
      QStringLiteral(
          "https://steamcommunity.com/sharedfiles/filedetails/?id=%1")
          .arg(id));
}

void SteamSourcePanel::on_visit_custom() {
  if (custom_url_ == nullptr)
    return;
  const QString url = custom_url_->text().trimmed();
  if (url.isEmpty())
    return;
  if (!data_.open_url)
    return;
  data_.open_url(url);
}

void SteamSourcePanel::on_custom_url_toggled() {
  if (loading_)
    return;
  custom_url_->setEnabled(custom_url_toggle_->isChecked());
  visit_custom_->setEnabled(custom_url_toggle_->isChecked() &&
                            !custom_url_->text().isEmpty());
  set_meta_value("General", "hasCustomURL",
                 custom_url_toggle_->isChecked() ? QStringLiteral("true")
                                                 : QStringLiteral("false"));
}

void SteamSourcePanel::persist_fields() {
  if (loading_)
    return;
  if (mod_id_ == nullptr)
    return;
  const QString wid = mod_id_->text().trimmed();
  set_meta_value("SteamWorkshop", "workshop_id", wid);
  set_meta_value("GameModManager", "source_id", wid);
  // Keep source_type in sync when user links a manual folder
  if (!wid.isEmpty()) {
    // Only write source_type if it is not already steam, to avoid
    // overwriting a different provider's type unintentionally.
    // But for Steam panel we ensure it is set when linking.
    QString cur_type = meta_value("GameModManager", "source_type");
    if (cur_type.isEmpty() || cur_type == QLatin1String("steam")) {
      set_meta_value("GameModManager", "source_type", QStringLiteral("steam"));
    }
  }
  set_meta_value("General", "version", version_->text().trimmed());
  set_meta_value("General", "category", category_->text().trimmed());
}

void SteamSourcePanel::persist_custom_url() {
  if (loading_)
    return;
  if (custom_url_ == nullptr)
    return;
  set_meta_value("General", "url", custom_url_->text().trimmed());
  visit_custom_->setEnabled(custom_url_toggle_->isChecked() &&
                            !custom_url_->text().isEmpty());
}

} // namespace ui
