#pragma once

#include "ui/modinfo/mod_info_tab.h"

#include <QString>

#include <vector>

class QLabel;
class QLineEdit;
class QMenu;
class QTreeWidget;
class QTreeWidgetItem;

namespace ui {

// MO2's Conflicts tab (general view): three two-column lists - files this mod
// wins, files it loses, and files with no conflict. Each row shows File |
// Provider (all owner mods, comma-joined). Each list has a filter and a count.
// Right-click offers Open / Preview / Explore / Hide / Unhide (the latter
// recomputes conflicts and refreshes the dialog). Double-click honors the
// doubleClicksOpenPreviews setting (Workspace-co2): plain vs Ctrl swaps
// between OS-open and built-in preview, with fallback to OS-open.
class ConflictsInfoTab : public ModInfoTab {
  Q_OBJECT
public:
  explicit ConflictsInfoTab(QWidget *parent = nullptr);
  ~ConflictsInfoTab() override;

  void set_mod(const ModInfoData &data) override;

private:
  struct File {
    QString rel_path;  // relative to the mod dir
    QString display;   // data_subpath stripped
    QString provider;  // all owner mod folders, comma-joined
    QString abs_path;
    bool won = false;
  };

  struct Group {
    QTreeWidget *list = nullptr;
    QLineEdit *filter = nullptr;
    QLabel *count     = nullptr;
    std::vector<File> files;
  };

  void rebuild(Group &group);
  void apply_filter(Group &group);
  void show_menu(Group &group, const QPoint &pos);
  void open_or_preview(Group &group, QTreeWidgetItem *item);
  void on_hide(Group &group, bool hide);
  QString selected_path(Group &group) const;

  Group wins_;
  Group loses_;
  Group no_conflict_;
};

}  // namespace ui
