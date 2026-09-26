#include "ui/panels/plugin_view.h"
#include "ui/settings/settings.h"
#include "ui/widgets/mod_table_view.h"

#include "ui/theme/icon_manager.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QDropEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLCDNumber>
#include <QList>
#include <QMouseEvent>
#include <QPainter>
#include <QPair>
#include <QPushButton>
#include <QRect>
#include <QSet>
#include <QShowEvent>
#include <QSize>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace ui {

// --- MO2 PluginList::tooltipData parity (pluginlist.cpp:1492-1718) ---

// MO2 TruncateString (pluginlist.cpp:53-65): over-long fields are cut at
// 1024 chars with "..." appended. Applies to author, description, the master
// joins and the archive join - never to Origin or the version numbers.
static QString truncate_mo2(const QString &s) {
  QString t = s;
  if (t.length() > 1024) {
    t.truncate(1024);
    t += "...";
  }
  return t;
}

static QString missing_masters_html(const engine::GamePlugin &p) {
  // MO2 testMasters semantics (pluginlist.cpp:1342-1361): enabled plugins
  // only; a master is unset when absent from the list OR present-but-disabled.
  QStringList names;
  for (const auto &s : p.master_unset)
    names << QString::fromStdString(s);
  return "<br><b>" + PluginView::tr("Missing Masters") + "</b>: <b>" +
         truncate_mo2(names.join(", ")) + "</b>";
}

// Enabled Masters = masters minus master_unset (present AND enabled), joined
// ", " in case-insensitive alphabetical order (MO2 FileNameComparator set),
// truncated like every other free-text field.
static QString enabled_masters_html(const engine::GamePlugin &p) {
  QStringList enabled;
  for (const auto &m : p.masters) {
    const QString qm = QString::fromStdString(m);
    bool unset       = false;
    for (const auto &u : p.master_unset) {
      if (qm.compare(QString::fromStdString(u), Qt::CaseInsensitive) == 0) {
        unset = true;
        break;
      }
    }
    if (!unset)
      enabled << qm;
  }
  if (enabled.isEmpty())
    return {};
  std::sort(enabled.begin(), enabled.end(), [](const QString &a, const QString &b) {
    return a.compare(b, Qt::CaseInsensitive) < 0;
  });
  return "<br><b>" + PluginView::tr("Enabled Masters") +
         "</b>: " + truncate_mo2(enabled.join(", "));
}

static QString archives_html(const engine::GamePlugin &p) {
  QString archive_line;
  if (p.archives.size() < 6) {
    QStringList names;
    for (const auto &a : p.archives)
      names << QString::fromStdString(a);
    archive_line = truncate_mo2(names.join(", ")) + "<br>";
  }
  return "<br><b>" + PluginView::tr("Loads Archives") + "</b>: " + archive_line +
         PluginView::tr(
             "There are Archives connected to this plugin. Their assets "
             "will be added to your game, overwriting in case of conflicts "
             "following the plugin order. Loose files will always overwrite "
             "assets from Archives. (This flag only checks for Archives from "
             "the same mod as the plugin)");
}

static QString has_ini_html() {
  return "<br><b>" + PluginView::tr("Loads INI settings") + "</b>:<br>" +
         PluginView::tr("There is an ini file connected to this plugin. Its settings "
                        "will be added to your game settings, overwriting in case of "
                        "conflicts.");
}

static QString esl_html(const engine::GamePlugin &p) {
  const QString type = p.has_master_ext ? "ESM" : "ESP";
  return "<br><br>" +
         PluginView::tr("This %1 is flagged as a light plugin (ESL). It will adhere "
                        "to the %1 load order but the records will be loaded in ESL "
                        "space (FE/FF). You can have up to 4096 light plugins in "
                        "addition to other plugin types.")
             .arg(type);
}

static QString esh_html() {
  return "<br><br>" +
         PluginView::tr(
             "This ESM is flagged as a medium plugin (ESH). It adheres to "
             "the ESM load order but loads records in ESH space (FD). You "
             "can have 256 medium plugins in addition to other plugin types.");
}

static QString both_light_medium_warning_html() {
  return "<br><br>" +
         PluginView::tr(
             "WARNING: This plugin is both light and medium flagged. This "
             "could indicate that the file was saved improperly and may have "
             "mismatched record references. Use it at your own risk.");
}

static QString dummy_html() {
  return "<br><br>" +
         PluginView::tr("This is a dummy plugin. It contains no records and is "
                        "typically used to load a paired archive file.");
}

static QString force_disabled_html(const engine::GamePlugin &p) {
  // MO2 forceDisabled block (pluginlist.cpp:1624-1642), non-blueprint games:
  // an .esl the game cannot load gets the light-support sentence, everything
  // else the generic custom-loading sentence. Blueprint variants do not apply
  // (no blueprint-capable game supported).
  if (p.has_light_ext)
    return "<br><br>" +
           PluginView::tr("Light plugins (ESL) are not supported by this game.");
  return "<br><br>" +
         PluginView::tr("This game does not currently permit custom plugin "
                        "loading. There may be manual workarounds.");
}

// Diagnostics-provider messages (the GMM analogue of MO2's addInformation
// section). MO2 emits message HTML raw - LOOT messages routinely contain
// anchors - so no escaping here either.
static QString messages_ul_html(const engine::GamePlugin &p) {
  if (p.messages.empty())
    return {};
  QString tip = "<hr><ul style=\"margin-left:15px; -qt-list-indent: 0;\">";
  for (const auto &msg : p.messages)
    tip += "<li>" + QString::fromStdString(msg) + "</li>";
  tip += "</ul>";
  return tip;
}

// LOOT per-plugin bullets (MO2 PluginList::makeLootTooltip,
// pluginlist.cpp:1665-1718): incompatibilities, missing masters, messages
// (Warning:/Error: prefixed), dirty findings ("%1 found %2 ITM record(s)..."),
// clean findings ("Verified clean by %1") - wrapped once in the exact <ul>.
static QString loot_ul_html(const engine::LootReport &r) {
  QString s;
  for (const auto &f : r.incompatibilities) {
    const QString name = f.second.empty() ? QString::fromStdString(f.first)
                                          : QString::fromStdString(f.second);
    s += "<li>" + PluginView::tr("Incompatible with %1").arg(name) + "</li>";
  }
  for (const auto &m : r.missing_masters)
    s += "<li>" +
         PluginView::tr("Depends on missing %1").arg(QString::fromStdString(m)) +
         "</li>";
  for (const auto &m : r.messages) {
    QString prefix;
    if (m.level == "warning")
      prefix = PluginView::tr("Warning") + ": ";
    else if (m.level == "error")
      prefix = PluginView::tr("Error") + ": ";
    s += "<li>" + prefix + QString::fromStdString(m.text) + "</li>";
  }
  for (const auto &d : r.dirty) {
    const QString utility = d.cleaning_utility.empty()
                                ? QStringLiteral("?")
                                : QString::fromStdString(d.cleaning_utility);
    QString line          = PluginView::tr("%1 found %2 ITM record(s), %3 deleted "
                                           "reference(s) and %4 deleted navmesh(es).")
                                .arg(utility)
                                .arg(d.itm_records)
                                .arg(d.deleted_references)
                                .arg(d.deleted_navmeshes);
    if (!d.info.empty())
      line += " " + QString::fromStdString(d.info);
    s += "<li>" + line + "</li>";
  }
  for (const auto &c : r.clean) {
    const QString utility = c.cleaning_utility.empty()
                                ? QStringLiteral("?")
                                : QString::fromStdString(c.cleaning_utility);
    s += "<li>" + PluginView::tr("Verified clean by %1").arg(utility) + "</li>";
  }
  if (s.isEmpty())
    return {};
  return "<hr><ul style=\"margin-top:0px; padding-top:0px; margin-left:15px; "
         "-qt-list-indent: 0;\">" +
         s + "</ul>";
}

static QString locked_column_tooltip() {
  return PluginView::tr("This plugin's load order position is locked.");
}

static QVector<QPair<QString, QString>>
plugin_flag_fragments(const engine::GamePlugin &p) {
  // MO2 PluginList::iconData order (pluginlist.cpp:1720-1779), minus the
  // locked emblem (GMM keeps the separate Locked column) and blueprint
  // (no blueprint-capable game supported): warning, information, attachment,
  // archive, awaiting, run (+warning when light AND medium), dummy, dirty.
  // Per-emblem hover text is a GMM additive extra - MO2 serves the row
  // tooltip on every column instead (see plugin_tooltip_html).
  QVector<QPair<QString, QString>> frags;
  const bool problematic = !p.master_unset.empty() ||
                           !p.loot_report.incompatibilities.empty() ||
                           !p.loot_report.missing_masters.empty();
  if (problematic)
    frags << QPair<QString, QString>(QStringLiteral("warning"),
                                     !p.master_unset.empty()
                                         ? missing_masters_html(p)
                                         : loot_ul_html(p.loot_report));
  if (!p.messages.empty() || !p.loot_report.messages.empty())
    frags << QPair<QString, QString>(QStringLiteral("information"),
                                     messages_ul_html(p) + loot_ul_html(p.loot_report));
  if (p.has_ini)
    frags << QPair<QString, QString>(QStringLiteral("attachment"), has_ini_html());
  if (!p.archives.empty())
    frags << QPair<QString, QString>(QStringLiteral("archive"), archives_html(p));
  if (p.is_light_flagged && !p.has_light_ext)
    frags << QPair<QString, QString>(QStringLiteral("awaiting"), esl_html(p));
  if (p.is_medium_flagged)
    frags << QPair<QString, QString>(QStringLiteral("run"), esh_html());
  if (p.is_light_flagged && p.is_medium_flagged) {
    // MO2 appends a second warning icon after run (pluginlist.cpp:1752-1757).
    const QString warn = both_light_medium_warning_html();
    for (auto &f : frags) {
      if (f.first == QLatin1String("awaiting") || f.first == QLatin1String("run"))
        f.second += warn;
    }
    frags << QPair<QString, QString>(QStringLiteral("warning"), warn);
  }
  if (p.has_no_records)
    frags << QPair<QString, QString>(QStringLiteral("dummy"), dummy_html());
  if (!p.loot_report.dirty.empty())
    frags << QPair<QString, QString>(QStringLiteral("dirty"),
                                     loot_ul_html(p.loot_report));
  return frags;
}

static QString plugin_tooltip_html(const engine::GamePlugin &p) {
  // Exact MO2 PluginList::tooltipData emission order (pluginlist.cpp:1499-1660):
  // Origin, force lines, versions, author/description, masters, archives, INI,
  // type paragraphs, dummy paragraph, forceDisabled block, messages, LOOT.
  QString tip;
  tip += "<b>" + PluginView::tr("Origin") + "</b>: " +
         (p.owner_mod.empty()
              ? QStringLiteral("Data")  // MO2 shows the raw base data origin name
              : QString::fromStdString(p.owner_mod).toHtmlEscaped());

  if (p.force_loaded)
    tip += "<br><b><i>" +
           PluginView::tr(
               "This plugin can't be disabled or moved (enforced by the game).") +
           "</i></b>";

  if (p.force_enabled)
    tip += "<br><b><i>" +
           PluginView::tr("This plugin can't be disabled (enforced by the game).") +
           "</i></b>";

  if (p.form_version != 0)
    tip += "<br><b>" + PluginView::tr("Form Version") +
           "</b>: " + QString::number(p.form_version);

  tip += "<br><b>" + PluginView::tr("Header Version") +
         "</b>: " + QString::number(p.header_version);

  if (!p.author.empty())
    tip += "<br><b>" + PluginView::tr("Author") +
           "</b>: " + truncate_mo2(QString::fromStdString(p.author));

  if (!p.description.empty())
    tip += "<br><b>" + PluginView::tr("Description") +
           "</b>: " + truncate_mo2(QString::fromStdString(p.description));

  if (!p.master_unset.empty())
    tip += missing_masters_html(p);

  tip += enabled_masters_html(p);

  if (!p.archives.empty())
    tip += archives_html(p);

  if (p.has_ini)
    tip += has_ini_html();

  if (p.is_light_flagged && !p.has_light_ext) {
    tip += esl_html(p);
  } else if (p.is_medium_flagged && p.has_master_ext) {
    tip += esh_html();
  }

  if (p.is_light_flagged && p.is_medium_flagged)
    tip += both_light_medium_warning_html();

  if (p.has_no_records)
    tip += dummy_html();

  if (p.force_disabled)
    tip += force_disabled_html(p);

  tip += messages_ul_html(p);
  tip += loot_ul_html(p.loot_report);

  return tip;
}

static QIcon plugin_flag_icon(const QString &token) {
  auto &icons = engine::IconManager::instance();
  if (token == QLatin1String("warning"))
    return icons.resolve_icon("plugin-warning");
  if (token == QLatin1String("information"))
    return icons.resolve_icon("dialog-information");
  if (token == QLatin1String("awaiting"))
    return icons.resolve_icon("plugin-light");
  if (token == QLatin1String("run"))
    return icons.resolve_icon("plugin-medium");
  if (token == QLatin1String("locked"))
    return icons.resolve_icon("plugin-locked");
  if (token == QLatin1String("attachment"))
    return icons.resolve_icon("plugin-attachment");
  if (token == QLatin1String("archive"))
    return icons.resolve_icon("plugin-archive");
  if (token == QLatin1String("dummy"))
    return icons.resolve_icon("plugin-dummy");
  if (token == QLatin1String("dirty"))
    return icons.resolve_icon("edit-clear");
  return {};
}

// Column 4 (Locked): center the single lock pin icon.
class CenteredIconDelegate : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QIcon icon      = opt.icon;
    opt.icon              = QIcon();
    const QWidget *widget = option.widget;
    QStyle *style         = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
    if (icon.isNull())
      return;
    const QSize sz = icon.actualSize(option.rect.size());
    const QRect r =
        QStyle::alignedRect(option.direction, Qt::AlignCenter, sz, option.rect);
    const QIcon::Mode mode =
        (option.state & QStyle::State_Selected) ? QIcon::Selected : QIcon::Normal;
    icon.paint(painter, r, Qt::AlignCenter, mode, QIcon::Off);
  }
};

// --- PluginTable (QTableWidget subclass with drag-reorder + deselection) ---

class PluginView::PluginTable : public QTableWidget {
public:
  using QTableWidget::QTableWidget;

  std::function<void(int, int)> on_reorder;
  // Double-click routing, wired by PluginView. `owner_of_row` maps a table
  // row to the mod folder providing that plugin ("" = a game-Data row, which
  // is dropped before either action runs); the two actions then carry the
  // owner id. Mirrors ModView's Ctrl+Double-Click split.
  std::function<std::string(int)> owner_of_row;
  std::function<void(const std::string &)> on_mod_info;
  std::function<void(const std::string &)> on_reveal;

  // Anti-bounce guard (Workspace-8fy, shared with ModView).
  [[nodiscard]] bool checkbox_toggle_recent() const { return bounce_guard_.recent(); }

protected:
  void dropEvent(QDropEvent *event) override {
    const int from = currentRow();
    if (from < 0) {
      event->ignore();
      return;
    }
    const QModelIndex idx = indexAt(event->position().toPoint());
    int to;
    switch (dropIndicatorPosition()) {
    case QAbstractItemView::AboveItem:
    case QAbstractItemView::OnItem:
      to = idx.isValid() ? idx.row() : rowCount() - 1;
      break;
    case QAbstractItemView::BelowItem:
      to = idx.isValid() ? idx.row() + 1 : rowCount() - 1;
      break;
    default:
      to = rowCount() - 1;
      break;
    }
    if (from == to || from + 1 == to) {
      event->accept();
      return;
    }
    if (on_reorder)
      on_reorder(from, to);
    event->accept();
  }

  void mousePressEvent(QMouseEvent *event) override {
    press_was_selected_ = false;
    press_on_check_     = false;
    if (event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier) {
      const QModelIndex idx = indexAt(event->pos());
      press_was_selected_   = idx.isValid() && selectionModel()->isSelected(idx);
      press_on_check_       = idx.isValid() && idx.column() == 0 &&
                              check_indicator_rect(idx).contains(event->pos());
    }
    QTableWidget::mousePressEvent(event);
  }

  void mouseDoubleClickEvent(QMouseEvent *event) override {
    const QModelIndex idx = indexAt(event->pos());
    if (!idx.isValid())
      return;
    // Anti-bounce: a double-click landing within doubleClickInterval() of a
    // checkbox toggle is the second half of an aim, not an open request.
    if (bounce_guard_.recent())
      return;
    // A game-Data plugin owns no mod, so there is nothing to open or reveal.
    const std::string owner = owner_of_row ? owner_of_row(idx.row()) : std::string();
    if (owner.empty())
      return;
    // MO2 parity (pluginlist.cpp): Ctrl+Double-Click opens the OS file
    // manager at the owning mod's folder. Consumed so the plain
    // double-click (Mod Info) does not also fire - same split as the mod
    // list (ModView::mouseDoubleClickEvent).
    if (event->modifiers() & Qt::ControlModifier) {
      if (on_reveal)
        on_reveal(owner);
      return;
    }
    if (on_mod_info)
      on_mod_info(owner);
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier &&
        press_was_selected_ && !press_on_check_) {
      clearSelection();
      event->accept();
      return;
    }
    // Anti-bounce bookkeeping (Workspace-8fy, shared with ModView): note the
    // time when this release actually flipped the enable checkbox, so the
    // double-click that follows is swallowed. Programmatic toggles
    // (set_plugins / sync_enabled) never pass through here.
    const QModelIndex idx = indexAt(event->pos());
    const bool watching = event->button() == Qt::LeftButton && idx.isValid() &&
                          idx.column() == 0;
    // Copy the state, never hold the QTableWidgetItem*: the toggle emits
    // toggle_requested, and the controller behind it may rebuild the table
    // and delete the item (use-after-free). Re-looked up after the base call.
    const int row    = watching ? idx.row() : -1;
    const int before = watching && item(row, 0) ? int(item(row, 0)->checkState()) : -1;
    QTableWidget::mouseReleaseEvent(event);
    QTableWidgetItem *after_item = before >= 0 ? item(row, 0) : nullptr;
    if (after_item && int(after_item->checkState()) != before)
      bounce_guard_.arm();
  }

private:
  QRect check_indicator_rect(const QModelIndex &idx) const {
    QStyleOptionViewItem opt;
    opt.initFrom(this);
    opt.rect = visualRect(idx);
    opt.features |= QStyleOptionViewItem::HasCheckIndicator;
    return style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt, this);
  }

  bool press_was_selected_ = false;
  bool press_on_check_     = false;
  CheckboxBounceGuard bounce_guard_;
};

// --- PluginView -----------------------------------------------------------

QTableWidget *PluginView::table() const {
  return table_;
}

PluginView::PluginView(QWidget *parent) : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  table_ = new PluginTable(0, 5, this);
  table_->setHorizontalHeaderLabels(
      {tr("Plugin Name"), tr("Flags"), tr("Priority"), tr("Mod Index"), tr("Locked")});
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->verticalHeader()->setVisible(false);
  table_->setItemDelegateForColumn(
      1, new ui::FlagsDelegate(PluginView::kPluginFlagsRole,
                               PluginView::kPluginFlagTooltipsRole, table_));
  table_->setItemDelegateForColumn(4, new CenteredIconDelegate(table_));
  connect(table_->horizontalHeader(), &QHeaderView::sectionResized, this,
          [this](int logical, int, int) {
            if (logical == 1)
              relayout_flag_rows();
          });
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setAlternatingRowColors(true);
  table_->setDragDropMode(QAbstractItemView::InternalMove);
  table_->setDefaultDropAction(Qt::MoveAction);
  table_->setDragDropOverwriteMode(false);
  table_->setDropIndicatorShown(true);
  table_->on_reorder = [this](int from, int to) {
    emit reorder_requested(from, to);
  };
  // Double-click routing (MO2 parity): plain opens the owning mod's Mod
  // Info, Ctrl reveals the owning mod's folder. The table only reports the
  // owner id; what to do with it belongs to the controller.
  table_->owner_of_row = [this](int row) { return owner_mod_at(row); };
  table_->on_mod_info = [this](const std::string &owner) {
    emit mod_info_requested(owner);
  };
  table_->on_reveal = [this](const std::string &owner) {
    emit reveal_requested(owner);
  };
  connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
    if (syncing_ || !item || item->column() != 0)
      return;
    const int row = item->row();
    if (row < 0 || row >= static_cast<int>(names_.size()))
      return;
    emit toggle_requested(names_[static_cast<size_t>(row)],
                          item->checkState() == Qt::Checked);
  });

  // Header row: refresh button + counter.
  auto *header = new QHBoxLayout;
  header->setContentsMargins(4, 2, 4, 2);
  refresh_button_ = new QPushButton(tr("Refresh"), this);
  refresh_button_->setObjectName("pluginRefreshBtn");
  refresh_button_->setToolTip(tr("Re-scan plugins on disk and reload the list."));
  connect(refresh_button_, &QPushButton::clicked, this, [this]() {
    emit refresh_requested();
  });
  header->addWidget(refresh_button_);
  header->addStretch(1);
  counter_display_ = new QLCDNumber(this);
  counter_display_->setObjectName("mo2CounterLabel");
  counter_display_->setDigitCount(4);
  counter_display_->setSegmentStyle(QLCDNumber::Flat);
  // Flat segments using QPalette text color for clear contrast on any theme.
  {
    auto pal = counter_display_->palette();
    pal.setColor(QPalette::WindowText, pal.color(QPalette::Text));
    counter_display_->setPalette(pal);
  }
  header->addWidget(counter_display_);
  layout->addLayout(header);
  layout->addWidget(table_);

  refresh_counters();
}

void PluginView::set_plugins(const std::vector<engine::GamePlugin> &plugins) {
  syncing_ = true;
  table_->setRowCount(0);
  names_.clear();
  owners_.clear();
  rows_locked_.clear();
  rows_force_loaded_.clear();
  rows_type_.clear();
  names_.reserve(plugins.size());
  owners_.reserve(plugins.size());
  rows_locked_.reserve(plugins.size());
  rows_force_loaded_.reserve(plugins.size());
  rows_type_.reserve(plugins.size());
  table_->setRowCount(static_cast<int>(plugins.size()));

  const QColor fixed_color(Qt::gray);
  const QColor disabled_color(Qt::darkRed);

  for (int i = 0; i < static_cast<int>(plugins.size()); ++i) {
    const auto &p = plugins[static_cast<size_t>(i)];
    names_.push_back(p.name);
    owners_.push_back(p.owner_mod);
    rows_locked_.push_back(p.locked);
    rows_force_loaded_.push_back(p.force_loaded);
    if (p.is_medium_flagged) {
      rows_type_.push_back(PluginType::Medium);
    } else if (p.has_light_ext || p.is_light_flagged) {
      rows_type_.push_back(PluginType::Light);
    } else if (p.has_master_ext || p.is_master_flagged) {
      rows_type_.push_back(PluginType::Master);
    } else {
      rows_type_.push_back(PluginType::Regular);
    }

    auto *name       = new QTableWidgetItem(QString::fromStdString(p.name));
    Qt::ItemFlags nf = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    // MO2 PluginList::checkstateData/flags parity: forceLoaded and
    // forceEnabled rows render checked and cannot be toggled; forceDisabled
    // rows render unchecked and cannot be toggled either. Only forceLoaded
    // and forceDisabled rows lose drag (a locked row stays draggable in MO2,
    // but GMM keeps its own pinning here - see G25).
    if (p.force_loaded || p.force_enabled) {
      Qt::ItemFlags pinned = nf;
      if (p.force_enabled && !p.locked)
        pinned |= Qt::ItemIsDragEnabled;
      name->setFlags(pinned);
      name->setCheckState(Qt::Checked);
      if (p.force_loaded)
        name->setForeground(fixed_color);
    } else if (p.force_disabled) {
      name->setFlags(nf);
      name->setCheckState(Qt::Unchecked);
      name->setForeground(disabled_color);
    } else {
      nf |= Qt::ItemIsUserCheckable;
      if (!p.locked)
        nf |= Qt::ItemIsDragEnabled;
      name->setFlags(nf);
      name->setCheckState(p.enabled ? Qt::Checked : Qt::Unchecked);
    }
    if (p.has_master_ext || p.is_master_flagged || p.has_light_ext ||
        p.is_light_flagged || p.is_medium_flagged) {
      QFont f = name->font();
      if (p.has_master_ext || p.is_master_flagged || p.has_light_ext)
        f.setBold(true);
      if (p.is_light_flagged || p.has_light_ext)
        f.setItalic(true);
      if (p.is_medium_flagged)
        f.setUnderline(true);
      name->setFont(f);
    }

    const auto flag_frags = plugin_flag_fragments(p);
    QList<QIcon> flag_icons;
    QStringList flag_tips;
    flag_icons.reserve(flag_frags.size());
    flag_tips.reserve(flag_frags.size());
    for (const auto &f : flag_frags) {
      flag_icons << plugin_flag_icon(f.first);
      flag_tips << f.second;
    }

    const QString tooltip = plugin_tooltip_html(p);
    name->setToolTip(tooltip);
    table_->setItem(i, 0, name);

    auto *flags      = new QTableWidgetItem;
    Qt::ItemFlags ff = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (!p.force_loaded && !p.force_disabled && !p.locked)
      ff |= Qt::ItemIsDragEnabled;
    flags->setFlags(ff);
    if (!flag_icons.isEmpty())
      flags->setData(kPluginFlagsRole, QVariant::fromValue(flag_icons));
    if (!flag_tips.isEmpty())
      flags->setData(kPluginFlagTooltipsRole, QVariant::fromValue(flag_tips));
    // MO2 serves the same rich row tooltip on every column including Flags
    // (column-independent data()); the per-emblem fragments above stay as a
    // GMM additive extra answered by FlagsDelegate::helpEvent.
    flags->setToolTip(tooltip);
    table_->setItem(i, 1, flags);

    auto *prio       = new QTableWidgetItem(QString::number(p.priority));
    Qt::ItemFlags pf = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (!p.force_loaded && !p.force_disabled && !p.locked)
      pf |= Qt::ItemIsDragEnabled;
    prio->setFlags(pf);
    prio->setTextAlignment(Qt::AlignCenter);
    if (p.force_loaded)
      prio->setForeground(fixed_color);
    prio->setToolTip(tooltip);
    table_->setItem(i, 2, prio);

    auto *idx        = new QTableWidgetItem(QString::fromStdString(p.mod_index_text));
    Qt::ItemFlags xf = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (!p.force_loaded && !p.force_disabled && !p.locked)
      xf |= Qt::ItemIsDragEnabled;
    idx->setFlags(xf);
    idx->setTextAlignment(Qt::AlignCenter);
    if (p.force_loaded)
      idx->setForeground(fixed_color);
    idx->setToolTip(tooltip);
    table_->setItem(i, 3, idx);

    auto *lock       = new QTableWidgetItem;
    Qt::ItemFlags lf = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (!p.force_loaded && !p.force_disabled && !p.locked)
      lf |= Qt::ItemIsDragEnabled;
    lock->setFlags(lf);
    if (p.locked) {
      lock->setIcon(plugin_flag_icon(QLatin1String("locked")));
      lock->setToolTip(locked_column_tooltip());
    }
    table_->setItem(i, 4, lock);
  }
  syncing_ = false;
  apply_highlights();
  relayout_flag_rows();
  refresh_counters();
}

void PluginView::relayout_flag_rows() {
  const int col_width = table_->columnWidth(1);
  const int default_h = table_->verticalHeader()->defaultSectionSize();
  for (int i = 0; i < table_->rowCount(); ++i) {
    const auto *item = table_->item(i, 1);
    const QList<QIcon> icons =
        item ? item->data(kPluginFlagsRole).value<QList<QIcon>>() : QList<QIcon>();
    const QSize wrapped = ui::flags_wrapped_size(icons, col_width);
    const int h =
        wrapped.height() > 0 ? std::max(default_h, wrapped.height()) : default_h;
    table_->setRowHeight(i, h);
  }
}

void PluginView::sync_enabled(const std::vector<engine::GamePlugin> &plugins) {
  syncing_       = true;
  const int rows = std::min(static_cast<int>(plugins.size()), table_->rowCount());
  for (int i = 0; i < rows; ++i) {
    const auto &p          = plugins[static_cast<size_t>(i)];
    QTableWidgetItem *item = table_->item(i, 0);
    if (!item || p.force_loaded || p.force_enabled || p.force_disabled)
      continue;
    item->setCheckState(p.enabled ? Qt::Checked : Qt::Unchecked);
  }
  syncing_ = false;
  refresh_counters();
}

void PluginView::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  refresh_counters();
}

void PluginView::refresh_counters() {
  int activeMasterCount = 0, activeLightCount = 0;
  int activeMediumCount = 0, activeRegularCount = 0;
  int masterCount = 0, lightCount = 0;
  int mediumCount = 0, regularCount = 0;
  int activeVisibleCount = 0;

  for (int i = 0; i < table_->rowCount(); ++i) {
    if (static_cast<size_t>(i) >= rows_type_.size())
      break;
    QTableWidgetItem *item = table_->item(i, 0);
    const bool active      = item && item->checkState() == Qt::Checked;
    const bool visible     = !table_->isRowHidden(i);
    switch (rows_type_[static_cast<size_t>(i)]) {
    case PluginType::Medium:
      ++mediumCount;
      activeMediumCount += active;
      activeVisibleCount += visible && active;
      break;
    case PluginType::Light:
      ++lightCount;
      activeLightCount += active;
      activeVisibleCount += visible && active;
      break;
    case PluginType::Master:
      ++masterCount;
      activeMasterCount += active;
      activeVisibleCount += visible && active;
      break;
    case PluginType::Regular:
      ++regularCount;
      activeRegularCount += active;
      activeVisibleCount += visible && active;
      break;
    }
  }

  const int activeCount =
      activeMasterCount + activeMediumCount + activeLightCount + activeRegularCount;
  const int totalCount = masterCount + mediumCount + lightCount + regularCount;

  QString tip = QStringLiteral("<table cellspacing=\"6\">"
                               "<tr><th>%1</th><th>%2</th><th>%3</th></tr>"
                               "<tr><td>All plugins:</td><td align=\"right\">%4</td>"
                               "<td align=\"right\">%5</td></tr>"
                               "<tr><td>ESMs:</td><td align=\"right\">%6</td>"
                               "<td align=\"right\">%7</td></tr>"
                               "<tr><td>ESPs:</td><td align=\"right\">%8</td>"
                               "<td align=\"right\">%9</td></tr>"
                               "<tr><td>ESMs+ESPs:</td><td align=\"right\">%10</td>"
                               "<td align=\"right\">%11</td></tr>")
                    .arg(tr("Type"), tr("Active"), tr("Total"))
                    .arg(activeCount)
                    .arg(totalCount)
                    .arg(activeMasterCount)
                    .arg(masterCount)
                    .arg(activeRegularCount)
                    .arg(regularCount)
                    .arg(activeMasterCount + activeRegularCount)
                    .arg(masterCount + regularCount);
  if (mediumCount > 0)
    tip += tr("<tr><td>ESHs:</td><td align=\"right\">%1</td>"
              "<td align=\"right\">%2</td></tr>")
               .arg(activeMediumCount)
               .arg(mediumCount);
  tip += tr("<tr><td>ESLs:</td><td align=\"right\">%1</td>"
            "<td align=\"right\">%2</td></tr>")
             .arg(activeLightCount)
             .arg(lightCount);
  tip += QStringLiteral("</table>");

  counter_display_->display(activeVisibleCount);
  counter_display_->setToolTip(tip);
}

void PluginView::apply_highlights() {
  const QColor contained_color = Settings::instance().plugin_list_contained();
  const QColor master_color    = Settings::instance().plugin_list_master();
  for (int i = 0; i < table_->rowCount(); ++i) {
    if (static_cast<size_t>(i) >= names_.size())
      continue;
    const QString name      = QString::fromStdString(names_[static_cast<size_t>(i)]);
    const bool is_contained = contained_names_.contains(name);
    const bool is_master    = master_names_.contains(name);
    const QBrush brush      = is_contained ? QBrush(contained_color)
                              : is_master  ? QBrush(master_color)
                                           : QBrush();
    for (int c = 0; c < table_->columnCount(); ++c) {
      if (auto *item = table_->item(i, c))
        item->setBackground(brush);
    }
  }
}

void PluginView::set_contained_plugins(const QVector<QString> &contained) {
  contained_names_ = QSet<QString>(contained.begin(), contained.end());
  apply_highlights();
}

void PluginView::set_master_plugins(const QVector<QString> &masters) {
  master_names_ = QSet<QString>(masters.begin(), masters.end());
  apply_highlights();
}

QStringList PluginView::selected_plugin_names() const {
  QStringList names;
  if (!table_ || !table_->selectionModel())
    return names;
  const auto rows = table_->selectionModel()->selectedRows();
  for (const auto &idx : rows) {
    if (auto *item = table_->item(idx.row(), 0))
      names << item->text();
  }
  return names;
}

}  // namespace ui
