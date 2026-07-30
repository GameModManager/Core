#include "ui/widgets/right_panel.h"
#include "ui/widgets/exec_controls_bar.h"
#include "ui/widgets/right_filter_bar.h"
#include "ui/widgets/column_toggle_header.h"
#include "ui/panels/tab_panels.h"
#include "engine/registry/game_capabilities.h"

#include <QHeaderView>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ui {

static void setup_toggle_header(QTableWidget* table, const QStringList& labels) {
    if (!table) return;
    auto* header = new ColumnToggleHeaderView(Qt::Horizontal, table);
    header->set_column_labels(labels);
    table->setHorizontalHeader(header);
    header->setStretchLastSection(true);
    header->setSectionsMovable(true);
}

RightPanel::RightPanel(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    exec_controls_ = new ExecControlsBar(this);
    layout->addWidget(exec_controls_);

    tab_widget_ = new QTabWidget(this);
    tab_widget_->setTabPosition(QTabWidget::North);

    // Data tab is always present — create it once
    data_tab_ = new DataTab(tab_widget_);
    setup_toggle_header(data_tab_->table(), {"Path", "Size", "Mod"});
    // Path stretches to fill space; Size and Mod keep user-set width
    auto* data_hdr = data_tab_->table()->horizontalHeader();
    data_hdr->setStretchLastSection(false);
    data_hdr->setSectionResizeMode(0, QHeaderView::Stretch);
    data_hdr->setSectionResizeMode(1, QHeaderView::Interactive);
    data_hdr->setSectionResizeMode(2, QHeaderView::Interactive);
    tab_widget_->addTab(data_tab_, "Data");

    layout->addWidget(tab_widget_, 1);

    // Small gap so the tab pane's bottom border is visible
    layout->addSpacing(1);

    // Filter bar below tabs — persists across tab switches
    filter_bar_ = new RightFilterBar(this);
    layout->addWidget(filter_bar_);

    // Re-filter when the user switches tabs
    connect(tab_widget_, &QTabWidget::currentChanged, this, [this]() {
        apply_filter();
    });

    // Re-filter as the user types
    connect(filter_bar_, &RightFilterBar::filter_changed, this, [this]() {
        apply_filter();
    });
}

QTableWidget* RightPanel::current_table() const {
    auto* w = tab_widget_->currentWidget();
    if (!w) return nullptr;

    // Each tab type exposes table() — try common patterns
    if (auto* t = w->findChild<QTableWidget*>()) return t;
    return nullptr;
}

void RightPanel::apply_filter() {
    const QString text = filter_bar_->filter_text().trimmed().toLower();

    auto* table = current_table();
    if (table) {
        filter_bar_->apply_to(table);
        return;
    }

    // ConflictsTab uses QTreeWidget — filter top-level items
    auto* w = tab_widget_->currentWidget();
    if (!w) return;
    auto* tree = w->findChild<QTreeWidget*>();
    if (tree) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            auto* item = tree->topLevelItem(i);
            if (text.isEmpty()) {
                item->setHidden(false);
                continue;
            }
            bool match = false;
            for (int col = 0; col < tree->columnCount(); ++col) {
                if (item->text(col).toLower().contains(text)) {
                    match = true;
                    break;
                }
            }
            item->setHidden(!match);
        }
    }
}

void RightPanel::clear_tabs() {
    // Remove all tabs except Data (index 0)
    while (tab_widget_->count() > 1) {
        tab_widget_->removeTab(1);
    }
    for (auto& [key, widget] : tabs_) {
        delete widget;
    }
    tabs_.clear();
}

void RightPanel::ensure_tab(const std::string& capability, const QString& label) {
    if (tabs_.count(capability)) return;

    QWidget* tab = nullptr;

    if (capability == "plugins") {
        auto* t = new PluginsTab(tab_widget_);
        setup_toggle_header(t->table(), {"Plugin", "Status", "Masters"});
        tab = t;
    } else if (capability == "conflicts") {
        auto* t = new ConflictsTab(tab_widget_);
        tab = t;
    } else if (capability == "archives") {
        auto* t = new ArchivesTab(tab_widget_);
        setup_toggle_header(t->table(), {"Archive", "Size", "Priority"});
        tab = t;
    } else if (capability == "saves") {
        auto* t = new SavesTab(tab_widget_);
        setup_toggle_header(t->table(), {"Save", "Date", "Size"});
        tab = t;
    } else if (capability == "downloads") {
        auto* t = new DownloadsTab(tab_widget_);
        setup_toggle_header(t->table(), {"Name", "Source", "Status", "Size"});
        // Name stretches; Source/Status/Size keep user-set width on resize
        auto* hdr = t->table()->horizontalHeader();
        hdr->setStretchLastSection(false);
        hdr->setSectionResizeMode(0, QHeaderView::Stretch);
        hdr->setSectionResizeMode(1, QHeaderView::Interactive);
        hdr->setSectionResizeMode(2, QHeaderView::Interactive);
        hdr->setSectionResizeMode(3, QHeaderView::Interactive);
        hdr->resizeSection(1, 80);
        hdr->resizeSection(2, 100);
        hdr->resizeSection(3, 80);
        tab = t;
    }

    if (tab) {
        tabs_[capability] = tab;
        tab_widget_->addTab(tab, label);
    }
}

void RightPanel::set_game(const std::string& game_id) {
    current_game_id_ = game_id;
    clear_tabs();

    if (!capabilities_) return;

    auto caps = capabilities_->sorted_capabilities_for(game_id);

    // Remove Data from index 0 — re-add at correct sorted position
    tab_widget_->removeTab(0);

    for (const auto& info : caps) {
        if (info.capability == "data") {
            tab_widget_->addTab(data_tab_, "Data");
        } else {
            ensure_tab(info.capability, QString::fromStdString(info.display_name));
        }
    }
}

DownloadsTab* RightPanel::downloads_tab() const {
    auto it = tabs_.find("downloads");
    if (it != tabs_.end())
        return qobject_cast<DownloadsTab*>(it->second);
    return nullptr;
}

ConflictsTab* RightPanel::conflicts_tab() const {
    auto it = tabs_.find("conflicts");
    if (it != tabs_.end())
        return qobject_cast<ConflictsTab*>(it->second);
    return nullptr;
}

}  // namespace ui
