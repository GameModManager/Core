#include "ui/widgets/mod_table_view.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/settings/settings.h"

#include <QAbstractItemModel>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
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
