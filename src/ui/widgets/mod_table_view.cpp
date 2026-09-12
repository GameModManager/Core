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
#include <QPen>
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

IndentDelegate::IndentDelegate(int indent_depth_role, int is_last_child_role,
                               int is_separator_role, QWidget* parent)
    : QStyledItemDelegate(parent),
      indent_depth_role_(indent_depth_role),
      is_last_child_role_(is_last_child_role),
      is_separator_role_(is_separator_role) {}

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

    // indentShift is the pure gutter width (depth * kIndentStep). The checkbox
    // and connector lines use this. Centered text (separators with "Center text
    // on separators" on, the default) doubles so a centered row indents the
    // same full kIndentStep per level a left-aligned row does.
    int indentShift = depth * kIndentStep;
    int shift = indentShift;
    if (opt.displayAlignment & Qt::AlignHCenter)
        shift *= 2;

    // Measure checkbox width if present (needed for text offset past the
    // shifted checkbox).
    int checkboxWidth = 0;
    if (opt.features & QStyleOptionViewItem::HasCheckIndicator) {
        const QRect check =
            style->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt, widget);
        checkboxWidth = check.right() - opt.rect.left() + 1;
    }

    // Pass 1: full-width background only (no checkbox, no text, no focus).
    // The selection/alternate tint fills the entire cell so the indent gutter
    // is never a gap in the row highlight.
    QStyleOptionViewItem bg = opt;
    bg.text.clear();
    bg.icon = QIcon();
    bg.features &= ~QStyleOptionViewItem::HasDisplay;
    bg.features &= ~QStyleOptionViewItem::HasCheckIndicator;
    bg.state &= ~QStyle::State_HasFocus;
    style->drawControl(QStyle::CE_ItemViewItem, &bg, painter, widget);

    // Pass 2: checkbox shifted right by the indent gutter width. The checkbox
    // moves WITH the name text so nested rows look properly indented under
    // their parent.
    if (checkboxWidth > 0) {
        QStyleOptionViewItem checkOpt = opt;
        QRect checkRect =
            style->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt, widget);
        checkRect.moveLeft(opt.rect.left() + indentShift);
        checkOpt.rect = checkRect;
        // PE_IndicatorItemViewItemCheck reads State_On/State_Off from state,
        // not from checkState - initStyleOption only sets checkState.
        checkOpt.state &= ~(QStyle::State_On | QStyle::State_Off |
                             QStyle::State_NoChange);
        if (checkOpt.checkState == Qt::Checked)
            checkOpt.state |= QStyle::State_On;
        else if (checkOpt.checkState == Qt::PartiallyChecked)
            checkOpt.state |= QStyle::State_NoChange;
        else
            checkOpt.state |= QStyle::State_Off;
        style->drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck,
                             &checkOpt, painter, widget);
    }

    // Pass 3: text + icon shifted right past the shifted checkbox (right edge
    // stays put, so nothing bleeds into the next column). Checkbox suppressed -
    // Pass 2 already drew it at the shifted position.
    QStyleOptionViewItem content = opt;
    content.rect.setLeft(content.rect.left() + shift + checkboxWidth);
    if (content.rect.width() > 0) {
        content.features &= ~QStyleOptionViewItem::HasCheckIndicator;
        style->drawControl(QStyle::CE_ItemViewItem, &content, painter, widget);
    }

    // Pass 4: KDE-style tree connector lines in the indent gutter. Separators
    // are structural parents and never get connectors on their own rows; only
    // mod rows (non-separator children) get the visual nesting lines.
    const bool is_separator = index.data(is_separator_role_).toBool();
    if (!is_separator) {
        const int checkbox_left = opt.rect.left() + indentShift;
        const int row_top = opt.rect.top();
        const int row_bottom = opt.rect.bottom();
        const int row_mid = (row_top + row_bottom) / 2;
        const bool is_last_child = index.data(is_last_child_role_).toBool();

        const QPalette pal = widget ? widget->palette() : QApplication::palette();
        // ponytail: PlaceholderText is mid-gray on both light and dark themes;
        // QPalette::Mid is nearly invisible on dark backgrounds.
        painter->setPen(QPen(pal.color(QPalette::PlaceholderText), 1));

        for (int i = 0; i < depth; ++i) {
            const int x = opt.rect.left() + kIndentStep * i + kCenterOffset;
            if (i < depth - 1) {
                // Ancestor level: continuous vertical line through the row.
                painter->drawLine(x, row_top, x, row_bottom);
            } else {
                // Immediate parent level: horizontal from the vertical line
                // to the shifted checkbox (start of content area).
                if (is_last_child) {
                    // L-connector: vertical from top to center, then horizontal.
                    painter->drawLine(x, row_top, x, row_mid);
                    painter->drawLine(x, row_mid, checkbox_left, row_mid);
                } else {
                    // T-connector: vertical full height, horizontal at center.
                    painter->drawLine(x, row_top, x, row_bottom);
                    painter->drawLine(x, row_mid, checkbox_left, row_mid);
                }
            }
        }
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
            view_->model()->index(visible_rows[i], 0).data(ModList::kScrollMarkRole);
        if (!color_variant.canConvert<QColor>()) continue;
        const QColor color = color_variant.value<QColor>();
        if (!color.isValid()) continue;
        const int y = groove.top() + static_cast<int>(i * scale);
        painter.fillRect(QRect(groove.left() + 3, y, groove.width() - 6, 3), color);
    }
}

ModView::ModView(QWidget* parent)
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
    setItemDelegateForColumn(ModList::Conflicts,
                             new FlagsDelegate(ModList::kFlagIconsRole, 0, this));
    setItemDelegateForColumn(ModList::Flags,
                             new FlagsDelegate(ModList::kFlagIconsRole, 0, this));
    // Name column: nesting indentation (shifts the name right under its parent,
    // purely visual). Depth 0 renders exactly like the default cell. Tree
    // connector lines are drawn in the indent gutter for depth > 0.
    setItemDelegateForColumn(ModList::Name,
                             new IndentDelegate(ModList::kIndentDepthRole,
                                                ModList::kIsLastChildRole,
                                                ModList::kIsSeparatorRole,
                                                this));
}

void ModView::apply_scrollbar_policy() {
    setVerticalScrollBarPolicy(
        Settings::instance().color_separator_scrollbar()
            ? Qt::ScrollBarAlwaysOn
            : Qt::ScrollBarAsNeeded);
}

void ModView::setModel(QAbstractItemModel* model) {
    QTreeView::setModel(model);
    if (auto* marking = qobject_cast<ModMarkingScrollBar*>(verticalScrollBar()))
        marking->set_model(model);
}

void ModView::setHeader(QHeaderView* header) {
    QTreeView::setHeader(header);
    if (!header) return;
    // Flag icons wrap based on the Conflicts/Flags column width (growing the
    // row), so the cached row heights must follow the section while the user
    // drags it.
    connect(header, &QHeaderView::sectionResized, this,
            [this](int logical, int, int) {
                if (logical == ModList::Conflicts ||
                    logical == ModList::Flags)
                    scheduleDelayedItemsLayout();
            });
}

void ModView::mouseDoubleClickEvent(QMouseEvent* event) {
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

void ModView::dragEnterEvent(QDragEnterEvent* event) {
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

void ModView::dragMoveEvent(QDragMoveEvent* event) {
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

void ModView::dropEvent(QDropEvent* event) {
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

bool ModView::is_under_overwrite(const QString& path) const {
    const auto* model = qobject_cast<const ModList*>(this->model());
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
