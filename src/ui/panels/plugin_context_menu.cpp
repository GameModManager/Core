#include "ui/panels/plugin_context_menu.h"

#include <QInputDialog>
#include <QMenu>
#include <QWidget>

namespace engine::PluginDb {

ContextMenu::ContextMenu(QObject *parent) : QObject(parent) {}

void ContextMenu::set_rows(const std::vector<RowInfo> &rows) {
  rows_ = rows;
}

void ContextMenu::add_actions(QMenu &menu, int row, const std::vector<int> &selection) {
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
  // all three, so they get no actions at all. The lock pair is the one block
  // that reads the selection rather than the clicked row; see below.
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

  // Lock / Unlock load order. MO2 walks the whole SELECTION, counts only the
  // rows that are ENABLED, and offers one action per state it found
  // (pluginlistcontextmenu.cpp:56-77), so a selection spanning locked and
  // unlocked rows gets both entries instead of the one derived from whichever
  // row happened to be clicked. A disabled row is invisible to the decision
  // AND skipped when applying it (PluginListContextMenu::setESPLock, :130-137):
  // pinning the load position of a plugin that is not loaded is not a state
  // the load order can carry.
  //
  // Core rows stay out either way - the engine refuses to lock them, so the
  // action would be a dead control.
  std::vector<size_t> lockable;
  if (selection.empty()) {
    if (!core)
      lockable.push_back(r);
  } else {
    for (int sel : selection) {
      if (sel < 0 || sel >= nrows)
        continue;
      const auto s = static_cast<size_t>(sel);
      if (rows_[s].force_loaded || !rows_[s].enabled)
        continue;
      lockable.push_back(s);
    }
  }

  bool has_locked   = false;
  bool has_unlocked = false;
  for (size_t s : lockable) {
    if (rows_[s].locked)
      has_locked = true;
    else
      has_unlocked = true;
  }

  if (has_locked) {
    menu.addAction(tr("Unlock load order"), this, [this, lockable]() {
      for (size_t s : lockable)
        emit lock_requested(rows_[s].name, false);
    });
  }
  if (has_unlocked) {
    menu.addAction(tr("Lock load order"), this, [this, lockable]() {
      for (size_t s : lockable)
        emit lock_requested(rows_[s].name, true);
    });
  }

  // "Open Origin in Explorer" / "Open Origin Info..."
  // (pluginlistcontextmenu.cpp:89-112). MO2 offers both only when the
  // row's origin resolves to a mod, and hides them entirely for a game file
  // like Skyrim.esm. A plugin with no owning mod is that same case, so the
  // pair is off rather than present and inert.
  const std::string &owner = rows_[r].owner_mod;
  if (owner.empty())
    return;

  menu.addSeparator();
  menu.addAction(tr("Open Origin in Explorer"), this, [this, owner]() {
    emit open_origin_explorer_requested(owner);
  });
  // MO2 also requires a single non-foreign selection. There is no foreign
  // case here: every mod row this list can name has a Mod Info dialog, which
  // is the same one the plain double-click opens.
  auto *info = menu.addAction(tr("Open Origin Info..."), this, [this, owner]() {
    emit open_origin_info_requested(owner);
  });
  // Double-click or Enter repeats the default action, so MO2's "the most
  // likely thing" choice is preserved (pluginlistcontextmenu.cpp:104).
  menu.setDefaultAction(info);
}

}  // namespace engine::PluginDb
