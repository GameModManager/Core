#pragma once

#include <QDialog>
#include <QString>
#include <filesystem>
#include <string>
#include <vector>

class QFileSystemModel;
class QModelIndex;
class QTreeView;

namespace ui {

class TaskDialog;

// Testable seam for the Overwrite info-dialog delete confirmation
// (Workspace-t6z5): fills a TaskDialog with the Confirm title, the
// single-file ("Are you sure you want to delete ...?") or N-entries main
// text, the system-trash note as content, a Question icon, and Yes/No
// command links. Shared between the dialog and the dialog tests.
void configure_overwrite_delete_dialog(TaskDialog &dlg, int count,
                                       const QString &file_name);

// MO2 OverwriteInfoDialog port: a modeless file browser over the Overwrite
// folder. Right-click offers Open / Rename / New Folder / Delete (Delete also
// via the Del key). Deletions go to the system trash (engine::remove_path) and
// the mod-mapping root dir (e.g. "Data") is protected. One shared instance is
// kept alive by the caller (findChild "__overwriteDialog" pattern) so the
// contents survive across sessions of the dialog.
class OverwriteInfoDialog : public QDialog {
  Q_OBJECT
public:
  OverwriteInfoDialog(const std::filesystem::path &overwrite_dir,
                      const std::string &mods_subpath, QWidget *parent = nullptr);

  // Point the dialog at a (possibly new) overwrite path.
  void set_path(const std::filesystem::path &overwrite_dir);

  // True when the index is the mod-mapping root dir (e.g. "Data") or the
  // overwrite root itself - such entries are protected from deletion.
  bool is_mapping_root(const QModelIndex &index) const;

private slots:
  void show_context_menu(const QPoint &pos);
  void delete_selected();
  void rename_selected();
  void open_selected();
  void new_folder();

private:
  std::vector<QModelIndex> selected_indexes() const;

  QFileSystemModel *model_ = nullptr;
  QTreeView *view_         = nullptr;
  std::filesystem::path overwrite_dir_;
  std::string mods_subpath_;
};

}  // namespace ui
