#include "ui/widgets/file_dialog_memory.h"
#include "ui/settings/settings.h"

#include <QFileDialog>
#include <QFileInfo>

namespace ui {

QMap<QString, QString> FileDialogMemory::cache_;

void FileDialogMemory::restore() {
  cache_ = Settings::instance().recent_dirs();
}

void FileDialogMemory::save() {
  Settings::instance().set_recent_dirs(cache_);
}

QString FileDialogMemory::start_dir(const QString &id, const QString &dir) {
  if (!dir.isEmpty())
    return dir;
  return cache_.value(id);
}

void FileDialogMemory::remember(const QString &id, const QString &path) {
  if (!id.isEmpty() && !path.isEmpty())
    cache_.insert(id, path);
}

QString FileDialogMemory::get_open_file_name(const QString &id, QWidget *parent,
                                             const QString &caption, const QString &dir,
                                             const QString &filter) {
  const QString chosen =
      QFileDialog::getOpenFileName(parent, caption, start_dir(id, dir), filter);
  // A cancelled dialog returns an empty string; that is not a new location.
  if (!chosen.isEmpty())
    remember(id, QFileInfo(chosen).absolutePath());
  return chosen;
}

QString FileDialogMemory::get_save_file_name(const QString &id, QWidget *parent,
                                             const QString &caption, const QString &dir,
                                             const QString &filter) {
  const QString chosen =
      QFileDialog::getSaveFileName(parent, caption, start_dir(id, dir), filter);
  if (!chosen.isEmpty())
    remember(id, QFileInfo(chosen).absolutePath());
  return chosen;
}

QString FileDialogMemory::get_existing_directory(const QString &id, QWidget *parent,
                                                 const QString &caption,
                                                 const QString &dir) {
  const QString chosen =
      QFileDialog::getExistingDirectory(parent, caption, start_dir(id, dir));
  if (!chosen.isEmpty())
    remember(id, chosen);
  return chosen;
}

}  // namespace ui