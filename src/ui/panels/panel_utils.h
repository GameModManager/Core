#pragma once

// Small helpers shared by the right-panel tab widgets (Downloads/Data/Saves/
// Conflicts tabs). Split out of the former tab_panels.cpp god file so each
// panel file stays self-contained. Header-only inline so no extra TU is
// needed.

#include "engine/theme/icon_manager.h"

#include <QAbstractItemView>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHeaderView>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QStyle>
#include <QTableWidget>
#include <QTreeWidgetItem>
#include <QWidget>

#include <cstdint>

namespace ui {

// Helper: format bytes to human-readable string
inline QString format_size(int64_t bytes) {
    if (bytes < 0) return "?";
    const char* units[] = {"B", "KB", "MB", "GB"};
    int unit_idx = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && unit_idx < 3) {
        size /= 1024.0;
        unit_idx++;
    }
    if (unit_idx == 0)
        return QString::number(static_cast<int>(size)) + " " + units[unit_idx];
    return QString::number(size, 'f', 1) + " " + units[unit_idx];
}

// Helper to create a standard table
inline QTableWidget* make_table(int cols, const QStringList& headers, QWidget* parent) {
    auto* table = new QTableWidget(parent);
    table->setColumnCount(cols);
    table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    return table;
}

// File-type icon for a path (cached provider).
inline QIcon icon_for_file(const QString& file_path) {
    static QFileIconProvider prov;
    auto px = prov.icon(QFileInfo(file_path)).pixmap(16, 16);
    return QIcon(px);
}

// Folder icon resolved through the central IconManager.
inline QIcon folder_icon() {
    static QIcon folder;
    if (folder.isNull()) {
        folder = engine::IconManager::instance().resolve_icon("folder", QStyle::SP_DirIcon);
    }
    return folder;
}

}  // namespace ui
