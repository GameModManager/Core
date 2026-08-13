#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include "ui/main_window/main_window.h"

namespace ui {

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

} // namespace ui