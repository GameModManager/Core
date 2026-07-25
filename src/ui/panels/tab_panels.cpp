#include "ui/panels/tab_panels.h"

#include <QHeaderView>
#include <QTableWidget>
#include <QVBoxLayout>

namespace ui {

// Helper to create a standard table
static QTableWidget* make_table(int cols, const QStringList& headers, QWidget* parent) {
    auto* table = new QTableWidget(parent);
    table->setColumnCount(cols);
    table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    return table;
}

// --- PluginsTab ---
PluginsTab::PluginsTab(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    table_ = make_table(3, {"Plugin", "Status", "Masters"}, this);
    layout->addWidget(table_);
}

// --- ArchivesTab ---
ArchivesTab::ArchivesTab(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    table_ = make_table(3, {"Archive", "Size", "Priority"}, this);
    layout->addWidget(table_);
}

// --- DataTab ---
DataTab::DataTab(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    table_ = make_table(3, {"Path", "Size", "Mod"}, this);
    layout->addWidget(table_);
}

// --- SavesTab ---
SavesTab::SavesTab(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    table_ = make_table(3, {"Save", "Date", "Size"}, this);
    layout->addWidget(table_);
}

// --- DownloadsTab ---
DownloadsTab::DownloadsTab(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    table_ = make_table(4, {"Name", "Source", "Size", "Status"}, this);
    layout->addWidget(table_);
}

}  // namespace ui
