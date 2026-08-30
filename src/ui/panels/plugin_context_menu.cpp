#include "ui/panels/plugin_context_menu.h"

#include <QMenu>

namespace engine::PluginDb {

ContextMenu::ContextMenu(QObject* parent) : QObject(parent) {}

void ContextMenu::set_rows(const std::vector<RowInfo>& rows) {
    rows_ = rows;
}

void ContextMenu::add_actions(QMenu& menu, int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return;
    const size_t r = static_cast<size_t>(row);

    // MO2's lock actions (PluginListContextMenu): "Lock load order" for
    // unlocked plugins, "Unlock load order" for locked ones. Core rows cannot
    // be locked (the engine refuses).
    const bool locked = rows_[r].locked;
    const bool core = rows_[r].force_loaded;
    if (!locked && !core) {
        menu.addAction(tr("Lock load order"), this,
                       [this, r]() { emit lock_requested(rows_[r].name, true); });
    } else if (locked) {
        menu.addAction(tr("Unlock load order"), this,
                       [this, r]() { emit lock_requested(rows_[r].name, false); });
    }
}

}  // namespace engine::PluginDb
