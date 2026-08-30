#include "ui/panels/plugins_tab.h"
#include "ui/settings/settings.h"
#include "ui/widgets/mod_table_view.h"

#include "ui/theme/icon_manager.h"

#include <QAbstractItemView>
#include <QAction>
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
#include <QMenu>
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

// --- PluginsTab ---

// --- Flag-bound tooltip fragments (MO2 PluginList::tooltipData sub-blocks) ---
// Each fragment describes exactly one status emblem; the Flags column shows
// ONLY the fragment of the emblem under the cursor. plugin_tooltip_html()
// composes the same fragments (plus the non-flag header block) into the full
// per-plugin tooltip shown on the name/priority/mod-index cells.

static QString missing_masters_html(const engine::GamePlugin &p) {
  QStringList names;
  for (const auto &s : p.missing_masters)
    names << QString::fromStdString(s);
  return "<br><b>" + PluginsTab::tr("Missing Masters") + "</b>: <b>" +
         names.join(", ") + "</b>";
}

static QString archives_html(const engine::GamePlugin &p) {
  // Fewer than 6 archives are listed inline; more just get the paragraph.
  QString archive_line;
  if (p.archives.size() < 6) {
    QStringList names;
    for (const auto &a : p.archives)
      names << QString::fromStdString(a);
    archive_line = names.join(", ") + "<br>";
  }
  return "<br><b>" + PluginsTab::tr("Loads Archives") +
         "</b>: " + archive_line +
         PluginsTab::tr(
             "There are Archives connected to this plugin. Their assets "
             "will be added to your game, overwriting in case of conflicts "
             "following the plugin order. Loose files will always overwrite "
             "assets from Archives. (This flag only checks for Archives from "
             "the same mod as the plugin)");
}

static QString has_ini_html() {
  return "<br><b>" + PluginsTab::tr("Loads INI settings") + "</b>:<br>" +
         PluginsTab::tr(
             "There is an ini file connected to this plugin. Its settings "
             "will be added to your game settings, overwriting in case of "
             "conflicts.");
}

static QString esl_html(const engine::GamePlugin &p) {
  const QString type = p.has_master_ext ? "ESM" : "ESP";
  return "<br><br>" +
         PluginsTab::tr(
             "This %1 is flagged as a light plugin (ESL). It will adhere "
             "to the %1 load order but the records will be loaded in ESL "
             "space (FE/FF). You can have up to 4096 light plugins in "
             "addition to other plugin types.")
             .arg(type);
}

static QString esh_html() {
  return "<br><br>" +
         PluginsTab::tr(
             "This ESM is flagged as a medium plugin (ESH). It adheres to "
             "the ESM load order but loads records in ESH space (FD). You "
             "can have 256 medium plugins in addition to other plugin types.");
}

static QString both_light_medium_warning_html() {
  return "<br><br>" +
         PluginsTab::tr(
             "WARNING: This plugin is both light and medium flagged. This "
             "could indicate that the file was saved improperly and may have "
             "mismatched record references. Use it at your own risk.");
}

static QString dummy_html() {
  return "<br><br>" +
         PluginsTab::tr("This is a dummy plugin. It contains no records and is "
                        "typically used to load a paired archive file.");
}

// GMM-specific lock marker (MO2 has no lock flag): the load-order pin. It
// lives in its own rightmost column, not among the Flags emblems.
static QString locked_column_tooltip() {
  return PluginsTab::tr("This plugin's load order position is locked.");
}

// (token, html) for every emblem the row shows, in MO2 iconData() order. The
// Flags column stores icons and tooltips as two parallel lists built from this,
// so the per-icon hover text can never point at the wrong emblem. The lock is
// intentionally NOT here - it has its own column.
static QVector<QPair<QString, QString>>
plugin_flag_fragments(const engine::GamePlugin &p) {
  QVector<QPair<QString, QString>> frags;
  if (!p.missing_masters.empty())
    frags << QPair<QString, QString>(QStringLiteral("warning"),
                                     missing_masters_html(p));
  if (p.has_ini)
    frags << QPair<QString, QString>(QStringLiteral("attachment"),
                                     has_ini_html());
  if (!p.archives.empty())
    frags << QPair<QString, QString>(QStringLiteral("archive"),
                                     archives_html(p));
  if (p.is_light_flagged && !p.has_light_ext)
    frags << QPair<QString, QString>(QStringLiteral("awaiting"), esl_html(p));
  if (p.is_medium_flagged)
    frags << QPair<QString, QString>(QStringLiteral("run"), esh_html());
  if (p.has_no_records)
    frags << QPair<QString, QString>(QStringLiteral("dummy"), dummy_html());
  // The both-light-and-medium warning rides both of those emblems.
  if (p.is_light_flagged && p.is_medium_flagged) {
    const QString warn = both_light_medium_warning_html();
    for (auto &f : frags) {
      if (f.first == QLatin1String("awaiting") ||
          f.first == QLatin1String("run"))
        f.second += warn;
    }
  }
  return frags;
}

// Rich per-plugin tooltip, mirroring MO2's PluginList::tooltipData layout
// (modorganizer/src/pluginlist.cpp:1492): Origin, enforced-notes, header
// metadata, masters, archives/INI paragraphs, ESL/ESH warnings, dummy note and
// diagnostics messages. Shown identically on every non-flags column of the row.
static QString plugin_tooltip_html(const engine::GamePlugin &p) {
  auto truncate = [](const QString &s) {
    QString t = s;
    if (t.length() > 4096) {
      t.truncate(4096);
      t += "...";
    }
    return t;
  };

  QString tip;
  tip += "<b>" + PluginsTab::tr("Origin") + "</b>: " +
         (p.owner_mod.empty()
              ? PluginsTab::tr("Game Data")
              : QString::fromStdString(p.owner_mod).toHtmlEscaped());

  if (p.force_loaded)
    tip +=
        "<br><b><i>" +
        PluginsTab::tr(
            "This plugin can't be disabled or moved (enforced by the game).") +
        "</i></b>";

  if (p.form_version != 0) // Oblivion-style headers have no form version
    tip += "<br><b>" + PluginsTab::tr("Form Version") +
           "</b>: " + QString::number(p.form_version);

  tip += "<br><b>" + PluginsTab::tr("Header Version") +
         "</b>: " + QString::number(p.header_version);

  if (!p.author.empty())
    tip += "<br><b>" + PluginsTab::tr("Author") + "</b>: " +
           truncate(QString::fromStdString(p.author).toHtmlEscaped());

  if (!p.description.empty())
    tip += "<br><b>" + PluginsTab::tr("Description") + "</b>: " +
           truncate(QString::fromStdString(p.description).toHtmlEscaped());

  if (!p.missing_masters.empty())
    tip += missing_masters_html(p);

  // Enabled masters = declared masters minus the absent ones (MO2
  // std::set_difference over masterUnset).
  QStringList enabled;
  for (const auto &m : p.masters) {
    if (std::find(p.missing_masters.begin(), p.missing_masters.end(), m) ==
        p.missing_masters.end())
      enabled << QString::fromStdString(m);
  }
  if (!enabled.isEmpty())
    tip += "<br><b>" + PluginsTab::tr("Enabled Masters") +
           "</b>: " + enabled.join(", ");

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

  if (!p.messages.empty()) {
    tip += "<hr><ul style=\"margin-left:15px; -qt-list-indent: 0;\">";
    for (const auto &msg : p.messages)
      tip += "<li>" + QString::fromStdString(msg).toHtmlEscaped() + "</li>";
    tip += "</ul>";
  }

  return tip;
}

// Table subclass that turns a drop between rows into a reorder request
// instead of letting Qt's default InternalMove rearrange items (whose order
// would then drift from the engine's). The engine repopulates on reorder.
// Defined at namespace scope (NOT in an anonymous namespace) so the qualified
// name matches the forward declaration in the header.
class PluginsTab::PluginTable : public QTableWidget {
public:
  using QTableWidget::QTableWidget;

  // Invoked with (from_row, to_row) when a valid reorder drop happens.
  std::function<void(int, int)> on_reorder;

protected:
  void dropEvent(QDropEvent *event) override {
    // The dragged row. Because the base dropEvent is never called (the
    // engine repopulates instead), the model isn't mutated during the
    // drag, so currentRow() still points at the row the user grabbed.
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
    default: // OnViewport
      to = rowCount() - 1;
      break;
    }
    if (from == to || from + 1 == to) { // no-op (incl. drop right below itself)
      event->accept();
      return;
    }
    if (on_reorder)
      on_reorder(from, to);
    event->accept(); // base dropEvent is NOT called: MainWindow repopulates
  }

  // MO2-style deselection: a plain left click on an already-selected row
  // clears the selection (click the selected plugin again -> unselected).
  // Clicks on the enable checkbox (column 0's check indicator) only toggle
  // the check state, keeping the selection, as in MO2. The "was it selected
  // before this click" test must happen at press time (the base press would
  // otherwise select the row before the release can inspect it); the clear
  // happens on release so drag-reorder still starts from a selected row.
  void mousePressEvent(QMouseEvent *event) override {
    press_was_selected_ = false;
    press_on_check_ = false;
    if (event->button() == Qt::LeftButton &&
        event->modifiers() == Qt::NoModifier) {
      const QModelIndex idx = indexAt(event->pos());
      press_was_selected_ = idx.isValid() && selectionModel()->isSelected(idx);
      press_on_check_ = idx.isValid() && idx.column() == 0 &&
                        check_indicator_rect(idx).contains(event->pos());
    }
    QTableWidget::mousePressEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton &&
        event->modifiers() == Qt::NoModifier && press_was_selected_ &&
        !press_on_check_) {
      clearSelection();
      event->accept();
      return;
    }
    QTableWidget::mouseReleaseEvent(event);
  }

private:
  QRect check_indicator_rect(const QModelIndex &idx) const {
    QStyleOptionViewItem opt;
    opt.initFrom(this);
    opt.rect = visualRect(idx);
    opt.features |= QStyleOptionViewItem::HasCheckIndicator;
    return style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt,
                                   this);
  }

  bool press_was_selected_ = false;
  bool press_on_check_ = false;
};

// MO2-style status emblems for the plugin list Flags column. One QIcon per
// emblem (MO2 renders a horizontal stack via its icon delegate; a QTableWidget
// cell can only carry one DecorationRole icon, so the emblems ride a custom
// role and a FlagsDelegate paints them individually at native size - stacking
// them into one pixmap would make Qt scale the whole stack down to one icon
// slot, the mod-list bug from 2026-08-03). Emblem names mirror MO2's
// iconData() tokens; per-flag hover text comes from plugin_flag_fragments().
static QIcon plugin_flag_icon(const QString &token) {
  auto &icons = engine::IconManager::instance();
  if (token == QLatin1String("warning"))
    return icons.resolve_icon("plugin-warning");
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
  return {};
}

// Column 4 (Locked): QTableWidget draws a bare DecorationRole icon at the
// cell's left edge no matter the item's textAlignment, so center the single
// lock pin by painting the decoration ourselves on top of the default path
// (background / selection / alternating rows still come from the base).
class CenteredIconDelegate : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QIcon icon = opt.icon;
    opt.icon = QIcon(); // base path draws no decoration
    // drawControl directly: QStyledItemDelegate::paint would re-run
    // initStyleOption and re-add the icon (left-aligned) on top of ours.
    const QWidget *widget = option.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
    if (icon.isNull())
      return;
    const QSize sz = icon.actualSize(option.rect.size());
    const QRect r =
        QStyle::alignedRect(option.direction, Qt::AlignCenter, sz, option.rect);
    const QIcon::Mode mode = (option.state & QStyle::State_Selected)
                                 ? QIcon::Selected
                                 : QIcon::Normal;
    icon.paint(painter, r, Qt::AlignCenter, mode, QIcon::Off);
  }
};

QTableWidget *PluginsTab::table() const { return table_; }

PluginsTab::PluginsTab(QWidget *parent) : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  table_ = new PluginTable(0, 5, this);
  table_->setHorizontalHeaderLabels({tr("Plugin Name"), tr("Flags"),
                                     tr("Priority"), tr("Mod Index"),
                                     tr("Locked")});
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->verticalHeader()->setVisible(false);
  // Flags column: one QIcon per emblem under kPluginFlagsRole, painted by the
  // shared FlagsDelegate at native size (a single DecorationRole icon would
  // have squeezed the whole stack into one slot). kPluginFlagTooltipsRole
  // gives the delegate per-emblem hover text. Row heights follow the wrap
  // math when the column is too narrow for every emblem.
  table_->setItemDelegateForColumn(
      1, new ui::FlagsDelegate(PluginsTab::kPluginFlagsRole,
                               PluginsTab::kPluginFlagTooltipsRole, table_));
  // Locked column centers its single pin (bare icons would sit left-aligned).
  table_->setItemDelegateForColumn(4, new CenteredIconDelegate(table_));
  connect(table_->horizontalHeader(), &QHeaderView::sectionResized, this,
          [this](int logical, int, int) {
            if (logical == 1)
              relayout_flag_rows();
          });
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setAlternatingRowColors(true);
  // Drag-reorder within the user band only; fixed rows are not draggable.
  table_->setDragDropMode(QAbstractItemView::InternalMove);
  table_->setDefaultDropAction(Qt::MoveAction);
  table_->setDragDropOverwriteMode(false);
  table_->setDropIndicatorShown(true);
  table_->on_reorder = [this](int from, int to) {
    emit reorder_requested(from, to);
  };
  connect(table_, &QTableWidget::itemChanged, this,
          [this](QTableWidgetItem *item) {
            if (syncing_ || !item || item->column() != 0)
              return;
            const int row = item->row();
            if (row < 0 || row >= static_cast<int>(names_.size()))
              return;
            emit toggle_requested(names_[static_cast<size_t>(row)],
                                  item->checkState() == Qt::Checked);
          });
  table_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(table_, &QTableWidget::customContextMenuRequested, this,
          &PluginsTab::on_custom_context_menu);

  // Extracted context menu (lock/unlock actions).
  context_menu_ = std::make_unique<engine::PluginDb::ContextMenu>(this);
  connect(context_menu_.get(), &engine::PluginDb::ContextMenu::lock_requested,
          this, &PluginsTab::lock_requested);

  // Header row: MO2-style refresh button (left) + active-plugin counter
  // (right, PluginListView::updatePluginCount parity).
  auto *header = new QHBoxLayout;
  header->setContentsMargins(4, 2, 4, 2);
  refresh_button_ = new QPushButton(tr("Refresh"), this);
  refresh_button_->setObjectName("pluginRefreshBtn");
  refresh_button_->setToolTip(
      tr("Re-scan plugins on disk and reload the list."));
  connect(refresh_button_, &QPushButton::clicked, this,
          [this]() { emit refresh_requested(); });
  header->addWidget(refresh_button_);
  header->addStretch(1);
  // MO2 renders this counter as a QLCDNumber (Qt's bundled seven-segment
  // "lcd" font inside a sunken box) — the [ 0000 ] look the user wanted.
  counter_display_ = new QLCDNumber(this);
  counter_display_->setObjectName("mo2CounterLabel");
  counter_display_->setDigitCount(4);
  header->addWidget(counter_display_);
  layout->addLayout(header);
  layout->addWidget(table_);

  refresh_counters(); // empty table -> 0 plus the breakdown tooltip
}

void PluginsTab::set_plugins(const std::vector<engine::GamePlugin> &plugins) {
  syncing_ = true;
  table_->setRowCount(0);
  names_.clear();
  rows_locked_.clear();
  rows_force_loaded_.clear();
  rows_type_.clear();
  names_.reserve(plugins.size());
  rows_locked_.reserve(plugins.size());
  rows_force_loaded_.reserve(plugins.size());
  rows_type_.reserve(plugins.size());
  table_->setRowCount(static_cast<int>(plugins.size()));

  const QColor missing_color(0xB0, 0x30, 0x30);
  const QColor fixed_color(Qt::gray);

  for (int i = 0; i < static_cast<int>(plugins.size()); ++i) {
    const auto &p = plugins[static_cast<size_t>(i)];
    names_.push_back(p.name);
    rows_locked_.push_back(p.locked);
    rows_force_loaded_.push_back(p.force_loaded);
    // MO2 counter classification order (updatePluginCount): medium wins,
    // then light (extension or flag), then master (extension or flag),
    // everything else is a regular plugin.
    if (p.is_medium_flagged) {
      rows_type_.push_back(PluginType::Medium);
    } else if (p.has_light_ext || p.is_light_flagged) {
      rows_type_.push_back(PluginType::Light);
    } else if (p.has_master_ext || p.is_master_flagged) {
      rows_type_.push_back(PluginType::Master);
    } else {
      rows_type_.push_back(PluginType::Regular);
    }

    // Column 0: name with the enable checkbox folded into the cell
    // (MO2-style). Fixed rows show a checked box that cannot be toggled;
    // missing-master rows can still be checked but the engine rejects the
    // enable with a message.
    auto *name = new QTableWidgetItem(QString::fromStdString(p.name));
    Qt::ItemFlags nf = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (p.force_loaded) {
      name->setFlags(nf); // no checkable, no drag: pinned
      name->setCheckState(Qt::Checked);
      name->setForeground(fixed_color);
    } else {
      nf |= Qt::ItemIsUserCheckable;
      if (!p.locked)
        nf |= Qt::ItemIsDragEnabled; // locked = immovable
      name->setFlags(nf);
      name->setCheckState(p.enabled ? Qt::Checked : Qt::Unchecked);
    }
    // MO2-style type indication via the name font (PluginList::fontData
    // parity): bold = master/light extension, italic = light, underline =
    // medium. Applies to fixed rows too (natives are bold masters).
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
    if (p.missing_master) {
      QFont f = name->font();
      f.setItalic(true);
      name->setFont(f);
      name->setForeground(missing_color);
    }

    // Emblems (MO2 PluginList::iconData token order) with their per-flag
    // hover text, both built from plugin_flag_fragments() so icons and
    // tooltips stay parallel. The name font carries the plugin type
    // instead. The flags cell shows ONLY the emblem's own info on hover;
    // the name/priority/mod-index cells keep the full rich tooltip.
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

    // Column 1: status emblems.
    auto *flags = new QTableWidgetItem;
    Qt::ItemFlags ff = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (!p.force_loaded && !p.locked)
      ff |= Qt::ItemIsDragEnabled;
    flags->setFlags(ff);
    if (!flag_icons.isEmpty())
      flags->setData(kPluginFlagsRole, QVariant::fromValue(flag_icons));
    if (!flag_tips.isEmpty())
      flags->setData(kPluginFlagTooltipsRole, QVariant::fromValue(flag_tips));
    flags->setToolTip(QString()); // per-emblem hover only, no full-row tooltip
    table_->setItem(i, 1, flags);

    // Column 2: priority.
    auto *prio = new QTableWidgetItem(QString::number(p.priority));
    Qt::ItemFlags pf = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (!p.force_loaded && !p.locked)
      pf |= Qt::ItemIsDragEnabled;
    prio->setFlags(pf);
    prio->setTextAlignment(Qt::AlignCenter);
    if (p.force_loaded)
      prio->setForeground(fixed_color);
    prio->setToolTip(tooltip);
    table_->setItem(i, 2, prio);

    // Column 3: mod index.
    auto *idx = new QTableWidgetItem(QString::fromStdString(p.mod_index_text));
    Qt::ItemFlags xf = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (!p.force_loaded && !p.locked)
      xf |= Qt::ItemIsDragEnabled;
    idx->setFlags(xf);
    idx->setTextAlignment(Qt::AlignCenter);
    if (p.force_loaded)
      idx->setForeground(fixed_color);
    idx->setToolTip(tooltip);
    table_->setItem(i, 3, idx);

    // Column 4: load-order lock (GMM-specific, not in MO2's emblem set).
    // Icon-only cell, empty for unlocked rows; CenteredIconDelegate centers
    // the pin that used to ride the Flags column.
    auto *lock = new QTableWidgetItem;
    Qt::ItemFlags lf = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (!p.force_loaded && !p.locked)
      lf |= Qt::ItemIsDragEnabled;
    lock->setFlags(lf);
    if (p.locked) {
      lock->setIcon(plugin_flag_icon(QLatin1String("locked")));
      lock->setToolTip(locked_column_tooltip());
    }
    table_->setItem(i, 4, lock);
  }
  syncing_ = false;
  apply_highlights();   // rows were rebuilt; re-tint selected-mod/master rows
  relayout_flag_rows(); // row heights follow the emblem wrap math
  // Populate the extracted context menu with per-row metadata.
  std::vector<engine::PluginDb::RowInfo> row_infos;
  row_infos.reserve(names_.size());
  for (size_t i = 0; i < names_.size(); ++i) {
    engine::PluginDb::RowInfo ri;
    ri.name = names_[i];
    ri.locked = i < rows_locked_.size() && rows_locked_[i];
    ri.force_loaded = i < rows_force_loaded_.size() && rows_force_loaded_[i];
    row_infos.push_back(std::move(ri));
  }
  context_menu_->set_rows(std::move(row_infos));
  refresh_counters();
}

void PluginsTab::relayout_flag_rows() {
  const int col_width = table_->columnWidth(1);
  const int default_h = table_->verticalHeader()->defaultSectionSize();
  for (int i = 0; i < table_->rowCount(); ++i) {
    const auto *item = table_->item(i, 1);
    const QList<QIcon> icons =
        item ? item->data(kPluginFlagsRole).value<QList<QIcon>>()
             : QList<QIcon>();
    const QSize wrapped = ui::flags_wrapped_size(icons, col_width);
    const int h = wrapped.height() > 0 ? std::max(default_h, wrapped.height())
                                       : default_h;
    table_->setRowHeight(i, h);
  }
}

void PluginsTab::sync_enabled(const std::vector<engine::GamePlugin> &plugins) {
  syncing_ = true;
  const int rows =
      std::min(static_cast<int>(plugins.size()), table_->rowCount());
  for (int i = 0; i < rows; ++i) {
    const auto &p = plugins[static_cast<size_t>(i)];
    QTableWidgetItem *item = table_->item(i, 0);
    if (!item || p.force_loaded)
      continue;
    item->setCheckState(p.enabled ? Qt::Checked : Qt::Unchecked);
  }
  syncing_ = false;
  refresh_counters(); // enable toggles change the counter in MO2 mode
}

void PluginsTab::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  refresh_counters(); // filter may have changed while another tab was current
}

void PluginsTab::refresh_counters() {
  int activeMasterCount = 0, activeLightCount = 0;
  int activeMediumCount = 0, activeRegularCount = 0;
  int masterCount = 0, lightCount = 0;
  int mediumCount = 0, regularCount = 0;
  int activeVisibleCount = 0;

  for (int i = 0; i < table_->rowCount(); ++i) {
    if (static_cast<size_t>(i) >= rows_type_.size())
      break;
    QTableWidgetItem *item = table_->item(i, 0);
    const bool active = item && item->checkState() == Qt::Checked;
    const bool visible = !table_->isRowHidden(i);
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

  const int activeCount = activeMasterCount + activeMediumCount +
                          activeLightCount + activeRegularCount;
  const int totalCount = masterCount + mediumCount + lightCount + regularCount;

  // MO2's breakdown tooltip (updatePluginCount): active/total per type.
  QString tip =
      QStringLiteral("<table cellspacing=\"6\">"
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

void PluginsTab::add_context_menu_actions(QMenu &menu, int row) {
  context_menu_->add_actions(menu, row);
}

void PluginsTab::on_custom_context_menu(const QPoint &pos) {
  const int row = table_->rowAt(pos.y());
  QMenu menu(this);
  add_context_menu_actions(menu, row);
  if (menu.actions().isEmpty())
    return;
  menu.exec(table_->viewport()->mapToGlobal(pos));
}

void PluginsTab::apply_highlights() {
  const QColor contained_color = Settings::instance().plugin_list_contained();
  const QColor master_color = Settings::instance().plugin_list_master();
  for (int i = 0; i < table_->rowCount(); ++i) {
    if (static_cast<size_t>(i) >= names_.size())
      continue;
    const QString name = QString::fromStdString(names_[static_cast<size_t>(i)]);
    const bool is_contained = contained_names_.contains(name);
    const bool is_master = master_names_.contains(name);
    // Contained wins over master, matching MO2's PluginList check order.
    // Rows in neither set are explicitly cleared (NoBrush) so a changed
    // selection never leaves stale tints from a previously selected mod.
    const QBrush brush = is_contained ? QBrush(contained_color)
                         : is_master  ? QBrush(master_color)
                                      : QBrush();
    for (int c = 0; c < table_->columnCount(); ++c) {
      if (auto *item = table_->item(i, c))
        item->setBackground(brush);
    }
  }
}

void PluginsTab::set_contained_plugins(const QVector<QString> &contained) {
  contained_names_ = QSet<QString>(contained.begin(), contained.end());
  apply_highlights();
}

void PluginsTab::set_master_plugins(const QVector<QString> &masters) {
  master_names_ = QSet<QString>(masters.begin(), masters.end());
  apply_highlights();
}

QStringList PluginsTab::selected_plugin_names() const {
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

} // namespace ui
