#include "ui/panels/plugin_context_menu.h"

#include <QInputDialog>
#include <QMenu>
#include <QWidget>

namespace engine::PluginDb {

ContextMenu::ContextMenu(QObject *parent) : QObject(parent) {}

void ContextMenu::set_rows(const std::vector<RowInfo> &rows) {
  rows_ = rows;
}

void ContextMenu::add_actions(QMenu &menu, int row) {
  // MO2 adds the Enable all / Disable all pair unconditionally
  // (pluginlistcontextmenu.cpp:36-49) - before the per-row block, and not
  // gated on a selection - so they are on the menu of every row AND on one
  // opened over empty space below the last row.
  menu.addAction(tr("Enable all"), this, [this]() {
    emit set_all_requested(true);
  });
  menu.addAction(tr("Disable all"), this, [this]() {
    emit set_all_requested(false);
  });

  if (row < 0 || row >= static_cast<int>(rows_.size()))
    return;
  const size_t r  = static_cast<size_t>(row);
  const int nrows = static_cast<int>(rows_.size());

  // MO2 PluginListContextMenu order (pluginlistcontextmenu.cpp:24-77):
  // Enable/Disable selected, Send to..., Lock/Unlock load order. Core rows
  // (force_loaded) cannot be toggled, moved or locked - the engine refuses
  // all three, so they get no actions at all.
  const bool locked = rows_[r].locked;
  const bool core   = rows_[r].force_loaded;

  if (!core) {
    const bool enable = !rows_[r].enabled;
    menu.addAction(tr(enable ? "Enable selected" : "Disable selected"), this,
                   [this, r, enable]() {
                     emit toggle_requested(rows_[r].name, enable);
                   });
  }

  // "Send to..." (MO2 createSendToContextMenu): Top is priority 0, Bottom is
  // the last row, "Priority..." asks for the target row. A locked row cannot
  // be moved, so the submenu is left off it.
  if (!locked && !core) {
    auto *send_to = menu.addMenu(tr("Send to..."));
    send_to->addAction(tr("Top"), this, [this, r]() {
      emit reorder_requested(static_cast<int>(r), 0);
    });
    send_to->addAction(tr("Bottom"), this, [this, r, nrows]() {
      emit reorder_requested(static_cast<int>(r), nrows - 1);
    });
    send_to->addAction(tr("Priority..."), this, [this, r, nrows]() {
      bool ok = false;
      // A row index is the priority (row 0 = loaded first), so the dialog
      // range is the list and the default is the row's own place.
      const int target =
          QInputDialog::getInt(qobject_cast<QWidget *>(parent()), tr("Set Priority"),
                               tr("Set the priority of the selected plugins"),
                               static_cast<int>(r), 0, nrows - 1, 1, &ok);
      if (ok)
        emit reorder_requested(static_cast<int>(r), target);
    });
  }

  if (!locked && !core) {
    menu.addAction(tr("Lock load order"), this, [this, r]() {
      emit lock_requested(rows_[r].name, true);
    });
  } else if (locked) {
    menu.addAction(tr("Unlock load order"), this, [this, r]() {
      emit lock_requested(rows_[r].name, false);
    });
  }
}

}  // namespace engine::PluginDb
