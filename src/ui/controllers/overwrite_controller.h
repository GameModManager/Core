#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include "ui/main_window/main_window.h"

namespace ui {

class TaskDialog;

// Testable seam for the clear-overwrite confirmation (Workspace-t6z5): fills
// a TaskDialog with the Clear Overwrite title, the remove-all-files main
// text, the system-trash note as content, a Question icon, and Yes/No
// command links. Shared between the controller and the dialog tests.
void configure_clear_overwrite_dialog(TaskDialog &dlg);

// Overwrite-folder operations: clear, create mod from overwrite, move content
// to a mod, sync to mods, open in file manager, info dialog, and drop-to-mod
// moves. Split out of the 7211-line main_window.cpp (Issue #16).
class OverwriteController : public QObject {
  Q_OBJECT
public:
  explicit OverwriteController(MainWindow *w, QObject *parent = nullptr);

public slots:
  void clear_overwrite();
  void create_mod_from_overwrite();
  void move_overwrite_content_to_mod();
  void sync_overwrite_to_mods();
  void open_overwrite_in_file_manager();
  void show_overwrite_info_dialog();
  void move_dropped_overwrite_files(const QStringList &paths, int mod_row);

private:
  MainWindow *w_ = nullptr;
};

}  // namespace ui