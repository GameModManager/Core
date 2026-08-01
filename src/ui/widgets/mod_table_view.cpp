#include "ui/widgets/mod_table_view.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/settings/settings.h"

#include <QAbstractItemModel>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QUrl>

#include <algorithm>

namespace ui {

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

    if (!Settings::instance().color_separator_scrollbar()) return;
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

void ModTableView::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        bool has_archive = false;
        for (const auto& url : event->mimeData()->urls()) {
            if (url.isLocalFile() && is_supported_archive(url.toLocalFile())) {
                has_archive = true;
                break;
            }
        }
        if (has_archive) {
            event->acceptProposedAction();
            return;
        }
    }
    QTreeView::dragEnterEvent(event);
}

void ModTableView::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    QTreeView::dragMoveEvent(event);
}

void ModTableView::dropEvent(QDropEvent* event) {
    if (event->mimeData()->hasUrls()) {
        QStringList paths;
        for (const auto& url : event->mimeData()->urls()) {
            if (url.isLocalFile() && is_supported_archive(url.toLocalFile()))
                paths.append(url.toLocalFile());
        }
        if (!paths.isEmpty()) {
            emit files_dropped(paths);
            event->acceptProposedAction();
            return;
        }
    }
    QTreeView::dropEvent(event);
}

}  // namespace ui
