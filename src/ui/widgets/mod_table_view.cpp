#include "ui/widgets/mod_table_view.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
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
