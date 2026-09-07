#include "ui/widgets/save_info_dialog.h"

#include "ui/settings/settings.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLocale>
#include <QPixmap>
#include <QResizeEvent>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace
{

// QLabel has no built-in elide. A path row like the Save "File:" entry can
// be 80+ chars and would otherwise wrap (when wordWrap=true) or push the
// dialog wide. Re-elide on resize so the visible text always fits the cell.
class ElidedLabel : public QLabel
{
public:
  using QLabel::QLabel;
  void setFullText(const QString& s)
  {
    full_ = s;
    update_elision();
  }

protected:
  void resizeEvent(QResizeEvent* e) override
  {
    QLabel::resizeEvent(e);
    update_elision();
  }

private:
  void update_elision()
  {
    if (full_.isEmpty()) {
      return;
    }
    const QFontMetrics fm(fontMetrics());
    setText(fm.elidedText(full_, Qt::ElideMiddle, std::max(width(), 1)));
  }
  QString full_;
};

}  // namespace

namespace ui
{

namespace
{

  // Match PluginDatabase's case-insensitive lookup convention (Windows FS).
  // Inline copy of engine::to_lower to avoid leaking the engine helper into
  // this header.
  std::string lc(std::string s)
  {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
      return std::tolower(c);
    });
    return s;
  }

  QString tooltip_for_missing(const engine::SaveMissingAsset& ma)
  {
    if (ma.inactive) {
      return SaveInfoDialog::tr("Disabled in current load order");
    }
    if (!ma.providing_mods.empty()) {
      QStringList mods;
      for (const auto& m : ma.providing_mods) {
        mods << QString::fromStdString(m);
      }
      return SaveInfoDialog::tr("Not in load order. Available in: %1")
          .arg(mods.join(", "));
    }
    return SaveInfoDialog::tr("Not in load order and not installed");
  }

}  // namespace

SaveInfoDialog::SaveInfoDialog(const engine::SaveGame& save,
                               const std::vector<engine::GamePlugin>& plugins_snapshot,
                               const std::vector<engine::SaveMissingAsset>& missing,
                               QWidget* parent)
    : QDialog(parent), save_(save), plugins_(plugins_snapshot), missing_(missing)
{
  setWindowTitle(tr("Save Information"));
  setMinimumSize(640, 360);
  resize(760, 480);

  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(8, 8, 8, 8);
  outer->setSpacing(8);

  auto* columns = new QHBoxLayout();
  columns->setSpacing(12);

  // --- Left column: thumbnail on top, basic info underneath ---------------
  // Wrap the left column in a fixed-width container so a long file path (or
  // future wide row) cannot stretch the dialog. The right column gets all
  // remaining width via stretch 1.
  auto* leftContainer = new QWidget(this);
  leftContainer->setFixedWidth(340);
  auto* left = new QVBoxLayout(leftContainer);
  left->setContentsMargins(0, 0, 0, 0);
  left->setSpacing(8);

  auto* thumb = new QLabel(leftContainer);
  thumb->setAlignment(Qt::AlignCenter);
  thumb->setMinimumSize(320, 180);
  thumb->setMaximumHeight(220);
  build_thumbnail(thumb);
  left->addWidget(thumb);

  auto* info = new QWidget(leftContainer);
  build_basic_info(info);
  left->addWidget(info, 1);

  columns->addWidget(leftContainer, 0);

  // --- Right column: header + plugin list --------------------------------
  auto* right = new QVBoxLayout();
  right->setSpacing(4);

  plugin_header_ = new QLabel(this);
  plugin_header_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  right->addWidget(plugin_header_);

  auto* list = new QListWidget(this);
  list->setUniformItemSizes(true);
  list->setSelectionMode(QAbstractItemView::NoSelection);
  right->addWidget(list, 1);

  columns->addLayout(right, 1);
  outer->addLayout(columns, 1);

  // Build the header + rows now that the list is parented correctly.
  build_plugin_list(list);

  // --- Bottom: Close button ---------------------------------------------
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  outer->addWidget(buttons);
}

void SaveInfoDialog::build_thumbnail(QLabel* target) const
{
  // MO2 GamebryoSaveGameInfoWidget::screenshotLabel: the screenshot is
  // embedded bytes (RGBA on SE, RGB on LE) - decode straight from the
  // SaveGame buffer, no file I/O.
  if (save_.screenshot_width > 0 && save_.screenshot_height > 0 &&
      save_.screenshot.size() >= static_cast<std::size_t>(save_.screenshot_width) *
                                     save_.screenshot_height * 3) {
    const bool rgba =
        save_.screenshot.size() ==
        static_cast<std::size_t>(save_.screenshot_width) * save_.screenshot_height * 4;
    QImage img(static_cast<const uchar*>(save_.screenshot.data()),
               save_.screenshot_width, save_.screenshot_height,
               rgba ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
    QPixmap pm = QPixmap::fromImage(img).scaled(320, 180, Qt::KeepAspectRatio,
                                                Qt::SmoothTransformation);
    target->setPixmap(pm);
    return;
  }
  target->setText(tr("No preview"));
  QFont f = target->font();
  f.setItalic(true);
  target->setFont(f);
}

void SaveInfoDialog::build_basic_info(QWidget* container) const
{
  auto* form = new QFormLayout(container);
  form->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
  form->setHorizontalSpacing(8);
  form->setVerticalSpacing(2);
  form->setContentsMargins(0, 0, 0, 0);

  const auto add_row = [&](const QString& label, const QString& value) {
    auto* lbl = new QLabel(value, container);
    lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lbl->setWordWrap(true);
    form->addRow(new QLabel(QString("<b>%1</b>").arg(label), container), lbl);
  };

  add_row(tr("Character:"), QString::fromStdString(save_.pc_name));
  add_row(tr("Level:"), QString::number(save_.pc_level));
  add_row(tr("Location:"), QString::fromStdString(save_.pc_location));
  add_row(tr("Save #:"), QString::number(save_.save_number));
  add_row(tr("Time:"),
          QLocale().toString(QDateTime::fromSecsSinceEpoch(save_.creation_time),
                             QLocale::ShortFormat));

  // File row: long Skyrim/FO4 save filenames routinely run 50+ chars. Elide
  // middle so the user can still see the prefix and extension, and show the
  // full path on hover.
  {
    const QString full = QString::fromStdString(save_.file_path.string());
    auto* lbl          = new ElidedLabel(container);
    lbl->setFullText(QString::fromStdString(save_.file_path.filename().string()));
    lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lbl->setToolTip(full);
    form->addRow(new QLabel(tr("<b>File:</b>"), container), lbl);
  }

  add_row(tr("Has Script Extender Data:"),
          save_.has_script_extender_file() ? tr("Yes") : tr("No"));

  if (!save_.all_files.empty()) {
    add_row(tr("All files:"), QString::number(save_.all_files.size()));
  }

  if (!save_.overlay.empty()) {
    // v2.1+ plugin-supplied extra rows. The previous "Details" header was
    // a single-column FormLayout row that left a visual gap and gave no
    // context about which system owned the rows; the row keys (e.g.
    // "Quest:", "Cell:") already self-describe, so the header is dropped.
    for (const auto& row : save_.overlay) {
      auto* lbl =
          new QLabel(QString("<b>%1</b> %2")
                         .arg(QString::fromStdString(row.key).toHtmlEscaped(),
                              QString::fromStdString(row.value).toHtmlEscaped()),
                     container);
      lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
      lbl->setWordWrap(true);
      form->addRow(QString(), lbl);
    }
  }
}

void SaveInfoDialog::build_plugin_list(QListWidget* target) const
{
  // Build the case-insensitive name index from the load-order snapshot
  // once. Three states mirror MO2's resolver: present + (enabled || force
  // loaded) = active, present + !enabled = inactive, absent = missing.
  struct State
  {
    const engine::GamePlugin* plugin = nullptr;
    bool inactive                    = false;
  };
  std::unordered_map<std::string, State> by_name;
  by_name.reserve(plugins_.size());
  for (const auto& p : plugins_) {
    State s;
    s.plugin   = &p;
    s.inactive = !p.enabled && !p.force_loaded;
    by_name.emplace(lc(p.name), s);
  }

  // Same key the Saves tab's missing-assets resolver uses (lc(name)).
  std::unordered_map<std::string, const engine::SaveMissingAsset*> missing_by_name;
  for (const auto& m : missing_) {
    missing_by_name.emplace(lc(m.plugin_name), &m);
  }

  // Walk the save's plugin lists in order. A QListWidgetItem gets a check
  // or cross icon and a tooltip with the verdict.
  QStyle* style = this->style();
  int count     = 0;

  const auto push = [&](const std::string& name) {
    const auto key = lc(name);
    const auto it  = by_name.find(key);
    auto* item     = new QListWidgetItem(QString::fromStdString(name), target);
    if (it != by_name.end()) {
      item->setIcon(style->standardIcon(QStyle::SP_DialogApplyButton));
      const auto& gp = *it->second.plugin;
      QString tip =
          gp.owner_mod.empty()
              ? tr("Provided by the game")
              : tr("Provided by: %1").arg(QString::fromStdString(gp.owner_mod));
      if (!gp.enabled && !gp.force_loaded) {
        tip += tr("\n(Disabled in current load order)");
      }
      item->setToolTip(tip);
    } else {
      item->setIcon(style->standardIcon(QStyle::SP_DialogCancelButton));
      item->setForeground(QBrush(QColor(0xC0, 0x39, 0x2B)));
      const auto mit = missing_by_name.find(key);
      item->setToolTip(mit != missing_by_name.end() ? tooltip_for_missing(*mit->second)
                                                    : tr("Not in load order"));
    }
    target->addItem(item);
    ++count;
  };
  for (const auto& n : save_.plugins)
    push(n);
  for (const auto& n : save_.light_plugins)
    push(n);
  for (const auto& n : save_.medium_plugins)
    push(n);

  // Set the header label created in the ctor.
  if (plugin_header_) {
    plugin_header_->setText(tr("<b>Plugins (%1)</b>").arg(count));
  }

  if (count == 0) {
    // Stub save (no parser for this game) or empty plugin list: show a
    // single disabled placeholder so the panel isn't blank.
    auto* item =
        new QListWidgetItem(tr("No plugin data (no parser for this game)"), target);
    item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
  }
}

int SaveInfoDialog::exec()
{
  const auto geo = Settings::instance().saveinfo_window_geometry();
  if (!geo.isEmpty())
    restoreGeometry(geo);
  const int rc = QDialog::exec();
  Settings::instance().set_saveinfo_window_geometry(saveGeometry());
  return rc;
}

}  // namespace ui
