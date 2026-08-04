#include "ui/widgets/mod_table_view.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/settings/settings.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QHeaderView>
#include <QHelpEvent>
#include <QMimeData>
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
    setVerticalScrollBar(new ModMarkingScrollBar(this));
    apply_scrollbar_policy();
    // Flags column: render stacked flag icons at native size (see FlagsDelegate).
    // No tooltips role (second arg 0): mod rows keep the delegate's default
    // helpEvent so per-row descriptions still come from the item's tooltip.
    setItemDelegateForColumn(ModListModel::Flags,
                             new FlagsDelegate(ModListModel::kFlagIconsRole, 0, this));
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
    // Flag icons wrap based on the Flags column width (growing the row), so the
    // cached row heights must follow the section while the user drags it.
    connect(header, &QHeaderView::sectionResized, this,
            [this](int logical, int, int) {
                if (logical == ModListModel::Flags)
                    scheduleDelayedItemsLayout();
            });
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
