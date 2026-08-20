#include "ui/widgets/mod_table_view.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/settings/settings.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QHeaderView>
#include <QHelpEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QToolTip>
#include <QUrl>

#include <algorithm>

namespace ui {

static QList<QIcon> flags_for_index(const QModelIndex& index, int role) {
    return index.data(role).value<QList<QIcon>>();
}

FlagsDelegate::FlagsDelegate(int flag_icons_role, int flag_tooltips_role,
                             QWidget* parent)
    : QStyledItemDelegate(parent),
      flag_icons_role_(flag_icons_role),
      flag_tooltips_role_(flag_tooltips_role) {}

void FlagsDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const {
    // Background / selection / focus via the default path; the model no longer
    // returns a DecorationRole for the Flags cell, so nothing extra is drawn.
    QStyledItemDelegate::paint(painter, option, index);

    const QList<QIcon> icons = flags_for_index(index, flag_icons_role_);
    if (icons.isEmpty()) return;

    const QSize cell = flags_cell_size(icons);
    const int per_line =
        flags_icons_per_line(option.rect.width(), cell.width(), kFlagsSpacing);
    const int lines = flags_icon_lines(icons.size(), per_line);
    if (lines <= 0) return;

    const QIcon::Mode mode = (option.state & QStyle::State_Selected)
                                 ? QIcon::Selected
                                 : ((option.state & QStyle::State_Enabled)
                                        ? QIcon::Normal
                                        : QIcon::Disabled);

    const int step_x = cell.width() + kFlagsSpacing;
    const int block_h = lines * cell.height();
    const int y0 = option.rect.top() +
                   std::max(0, (option.rect.height() - block_h) / 2);
    for (int i = 0; i < icons.size(); ++i) {
        const QRect target(option.rect.left() + (i % per_line) * step_x,
                           y0 + (i / per_line) * cell.height(),
                           cell.width(), cell.height());
        icons[i].paint(painter, target, Qt::AlignCenter, mode, QIcon::Off);
    }
}

QSize FlagsDelegate::sizeHint(const QStyleOptionViewItem& option,
                              const QModelIndex& index) const {
    const QList<QIcon> icons = flags_for_index(index, flag_icons_role_);
    if (icons.isEmpty())
        return QStyledItemDelegate::sizeHint(option, index);

    // A hidden Flags column must not inflate row heights.
    if (const auto* view = qobject_cast<const QTreeView*>(option.widget)) {
        if (view->isColumnHidden(index.column()))
            return QStyledItemDelegate::sizeHint(option, index);
    }

    int cell_width = -1;
    if (const auto* view = qobject_cast<const QTreeView*>(option.widget))
        cell_width = view->columnWidth(index.column());
    if (cell_width <= 0) cell_width = 80;  // default Flags column width

    // Width -1 = no preference; QTreeView only uses the height for row layout.
    return flags_wrapped_size(icons, cell_width);
}

bool FlagsDelegate::helpEvent(QHelpEvent* event, QAbstractItemView* view,
                              const QStyleOptionViewItem& option,
                              const QModelIndex& index) {
    if (event->type() == QEvent::ToolTip && flag_tooltips_role_ != 0) {
        const QStringList tips =
            index.data(flag_tooltips_role_).value<QStringList>();
        if (!tips.isEmpty()) {
            const QList<QIcon> icons = flags_for_index(index, flag_icons_role_);
            const int i = flag_icon_at(icons, option.rect, event->pos());
            if (i >= 0 && i < tips.size()) {
                QToolTip::showText(event->globalPos(), tips[i], view);
                return true;
            }
            // Over the Flags cell but not on an emblem: no tooltip at all.
            return true;
        }
    }
    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

static bool is_supported_archive(const QString& path) {
    static const QStringList exts = {".zip", ".rar", ".7z", ".7zip", ".gz", ".tar"};
    for (const auto& ext : exts) {
        if (path.endsWith(ext, Qt::CaseInsensitive)) return true;
    }
    return false;
}

IndentDelegate::IndentDelegate(int indent_depth_role, QWidget* parent)
    : QStyledItemDelegate(parent), indent_depth_role_(indent_depth_role) {}

void IndentDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                           const QModelIndex& index) const {
    const int depth = index.data(indent_depth_role_).toInt();

    // Resolve the full option exactly like QStyledItemDelegate::paint would,
    // but ONCE, so each pass below controls which pieces it draws. (Calling
    // QStyledItemDelegate::paint re-runs initStyleOption per call and re-reads
    // CheckStateRole from the model, so clearing HasCheckIndicator on a pass
    // did NOT suppress its checkbox - the nested rows drew two checkboxes.)
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QWidget* widget = opt.widget ? opt.widget : nullptr;
    QStyle* style = widget ? widget->style() : QApplication::style();

    if (depth <= 0) {
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
        return;
    }
    int shift = depth * kIndentStep;
    // Centered text (separators with "Center text on separators" on, the
    // default) moves only HALF the rect shift: the text centers within
    // [L+shift, R], whose center is C + shift/2. Double the shift so a centered
    // row indents the same full kIndentStep per level a left-aligned row does -
    // without this, nested separators looked flat until ~4 levels deep.
    if (opt.displayAlignment & Qt::AlignHCenter)
        shift *= 2;
    // The checkbox stays at its normal (left) position, so the shifted name
    // must clear it - ADD the checkbox width rather than max()ing with it:
    // Pass 2 suppresses the checkbox, so the style reserves no space for it and
    // the name would otherwise start exactly where the PARENT's text starts
    // (the parent's own text is already past its checkbox). max(shift, cbw)
    // made the depth-1 shift degenerate to exactly the checkbox width, so the
    // first nested mod rendered ~0px past its parent ("mod 2 is 2-3px left of
    // its parent"); only depth 2+ stepped.
    if (opt.features & QStyleOptionViewItem::HasCheckIndicator) {
        const QRect check =
            style->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt, widget);
        shift += check.right() - opt.rect.left() + 1;
    }

    // Pass 1: full-width background + the checkbox at its NORMAL position (the
    // left edge, exactly where a flat row draws it) - no text/icon, so the
    // indentation gutter keeps the row highlight and the checkbox NEVER shifts
    // with the name.
    QStyleOptionViewItem bg = opt;
    bg.text.clear();
    bg.icon = QIcon();
    bg.features &= ~QStyleOptionViewItem::HasDisplay;
    bg.state &= ~QStyle::State_HasFocus;
    style->drawControl(QStyle::CE_ItemViewItem, &bg, painter, widget);

    // Pass 2: text + icon shifted right into the remaining width (right edge
    // stays put, so nothing bleeds into the next column). Checkbox suppressed -
    // Pass 1 already drew it at the normal position.
    QStyleOptionViewItem content = opt;
    content.rect.setLeft(content.rect.left() + shift);
    if (content.rect.width() > 0) {
        content.features &= ~QStyleOptionViewItem::HasCheckIndicator;
        style->drawControl(QStyle::CE_ItemViewItem, &content, painter, widget);
    }
}

ModMarkingScrollBar::ModMarkingScrollBar(QTreeView* view)
    : QScrollBar(view), view_(view) {}

void ModMarkingScrollBar::set_model(QAbstractItemModel* model) {
    // Repaint the marks whenever row data/visibility changes.
    if (model) {
        connect(model, &QAbstractItemModel::dataChanged, this, [this]() { update(); });
        connect(model, &QAbstractItemModel::layoutChanged, this, [this]() { update(); });
    }
}

void ModMarkingScrollBar::paintEvent(QPaintEvent* event) {
    QScrollBar::paintEvent(event);

    if (!view_ || !view_->model()) return;

    QStyleOptionSlider style_option;
    initStyleOption(&style_option);
    const QRect groove = style()->subControlRect(
        QStyle::CC_ScrollBar, &style_option, QStyle::SC_ScrollBarGroove, this);
    if (groove.height() <= 0) return;

    // Visible rows in model order (folded separator rows are hidden).
    QVector<int> visible_rows;
    const int row_count = view_->model()->rowCount();
    visible_rows.reserve(row_count);
    for (int r = 0; r < row_count; ++r) {
        if (!view_->isRowHidden(r, QModelIndex())) visible_rows.append(r);
    }
    if (visible_rows.isEmpty()) return;

    const qreal scale = static_cast<qreal>(groove.height()) / visible_rows.size();

    // The model gates separator marks behind color_separator_scrollbar(); this
    // pass draws whatever valid marks it reports (separators and/or
    // plugin-selected highlights), so highlights are navigable in huge lists
    // even with separator coloring off.
    QPainter painter(this);
    for (int i = 0; i < visible_rows.size(); ++i) {
        const QVariant color_variant =
            view_->model()->index(visible_rows[i], 0).data(ModListModel::kScrollMarkRole);
        if (!color_variant.canConvert<QColor>()) continue;
        const QColor color = color_variant.value<QColor>();
        if (!color.isValid()) continue;
        const int y = groove.top() + static_cast<int>(i * scale);
        painter.fillRect(QRect(groove.left() + 3, y, groove.width() - 6, 3), color);
    }
}

ModTableView::ModTableView(QWidget* parent)
    : QTreeView(parent) {
    setRootIsDecorated(false);
    setIndentation(0);
    setAlternatingRowColors(true);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    // Double-click opens the Mod Info popup (see ModListController), never
    // rename: the default QTreeView triggers include DoubleClicked, which
    // would open the inline rename editor on the Name column after the modal
    // info dialog closes (QAbstractItemView::mouseDoubleClickEvent emits
    // doubleClicked first and only runs the edit trigger once the slot
    // returns, i.e. after dlg.exec() unwinds). Rename stays reachable via
    // EditKeyPressed (F2) and the context menu's "Rename Mod..." entry.
    setEditTriggers(QAbstractItemView::EditKeyPressed);
    setVerticalScrollBar(new ModMarkingScrollBar(this));
    apply_scrollbar_policy();
    // Conflicts + Flags columns: render stacked flag icons at native size (see
    // FlagsDelegate). MO2 splits these into COL_CONFLICTFLAGS (win/loss badge)
    // and COL_FLAGS (hidden/FOMOD/root-override badges); both come through the
    // same kFlagIconsRole, filtered per column by the model. No tooltips role
    // (second arg 0): mod rows keep the delegate's default helpEvent so
    // per-row descriptions still come from the item's tooltip.
    setItemDelegateForColumn(ModListModel::Conflicts,
                             new FlagsDelegate(ModListModel::kFlagIconsRole, 0, this));
    setItemDelegateForColumn(ModListModel::Flags,
                             new FlagsDelegate(ModListModel::kFlagIconsRole, 0, this));
    // Name column: nesting indentation (shifts the name right under its parent,
    // purely visual). Depth 0 renders exactly like the default cell.
    setItemDelegateForColumn(ModListModel::Name,
                             new IndentDelegate(ModListModel::kIndentDepthRole, this));
}

void ModTableView::apply_scrollbar_policy() {
    setVerticalScrollBarPolicy(
        Settings::instance().color_separator_scrollbar()
            ? Qt::ScrollBarAlwaysOn
            : Qt::ScrollBarAsNeeded);
}

void ModTableView::setModel(QAbstractItemModel* model) {
    QTreeView::setModel(model);
    if (auto* marking = qobject_cast<ModMarkingScrollBar*>(verticalScrollBar()))
        marking->set_model(model);
}

void ModTableView::setHeader(QHeaderView* header) {
    QTreeView::setHeader(header);
    if (!header) return;
    // Flag icons wrap based on the Conflicts/Flags column width (growing the
    // row), so the cached row heights must follow the section while the user
    // drags it.
    connect(header, &QHeaderView::sectionResized, this,
            [this](int logical, int, int) {
                if (logical == ModListModel::Conflicts ||
                    logical == ModListModel::Flags)
                    scheduleDelayedItemsLayout();
            });
}

void ModTableView::mouseDoubleClickEvent(QMouseEvent* event) {
    // MO2 parity (modlistview.cpp): Ctrl+Double-Click opens the OS file
    // explorer at the mod's folder. The controller resolves the folder and
    // opens it; the event is consumed so the plain double-click behavior
    // (Mod Info popup) does not also fire.
    if (event->modifiers() & Qt::ControlModifier) {
        const QModelIndex index = indexAt(event->pos());
        if (index.isValid()) {
            emit ctrl_double_clicked(index);
            return;
        }
    }
    QTreeView::mouseDoubleClickEvent(event);
}

void ModTableView::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        for (const auto& url : event->mimeData()->urls()) {
            if (!url.isLocalFile()) continue;
            const auto path = url.toLocalFile();
            if (is_supported_archive(path) || is_under_overwrite(path)) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    QTreeView::dragEnterEvent(event);
}

void ModTableView::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasUrls()) {
        for (const auto& url : event->mimeData()->urls()) {
            if (!url.isLocalFile()) continue;
            const auto path = url.toLocalFile();
            if (is_supported_archive(path) || is_under_overwrite(path)) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    QTreeView::dragMoveEvent(event);
}

void ModTableView::dropEvent(QDropEvent* event) {
    if (event->mimeData()->hasUrls()) {
        QStringList archives;
        QStringList overwrite_paths;
        for (const auto& url : event->mimeData()->urls()) {
            if (!url.isLocalFile()) continue;
            const auto path = url.toLocalFile();
            if (is_supported_archive(path))
                archives.append(path);
            else if (is_under_overwrite(path))
                overwrite_paths.append(path);
        }

        if (!overwrite_paths.isEmpty()) {
            const int row = indexAt(event->position().toPoint()).row();
            if (row >= 0) {
                emit overwrite_files_dropped(overwrite_paths, row);
                event->acceptProposedAction();
                return;
            }
        }
        if (!archives.isEmpty()) {
            emit files_dropped(archives);
            event->acceptProposedAction();
            return;
        }
    }
    QTreeView::dropEvent(event);
}

bool ModTableView::is_under_overwrite(const QString& path) const {
    const auto* model = qobject_cast<const ModListModel*>(this->model());
    if (!model) return false;
    const auto ow = model->overwrite_path();
    if (ow.isEmpty()) return false;

    QFileInfo info(path);
    const auto canon = info.canonicalFilePath();
    const QFileInfo ow_info(ow);
    const auto ow_canon = ow_info.canonicalFilePath();
    if (canon.isEmpty() || ow_canon.isEmpty()) return false;

    // Equal to or strictly inside the overwrite dir.
    return canon == ow_canon ||
           canon.startsWith(ow_canon + QLatin1Char('/'));
}

}  // namespace ui
