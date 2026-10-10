#pragma once

#include <QMap>
#include <QString>
#include <QWidget>

namespace ui {

// MO2 FileDialogMemory (filedialogmemory.cpp, wired at mainwindow.cpp:478):
// a NAMED file dialog opens where it was last used, so browsing to a folder
// once does not have to be repeated. An explicit start directory always wins -
// the remembered one is a fallback, exactly as MO2 has it - but every dialog
// records where it ended up.
//
// The map lives here and is mirrored into the settings by save(); restore()
// reads it back at startup.
class FileDialogMemory {
public:
  static void restore();
  static void save();

  [[nodiscard]] static QString get_open_file_name(const QString &id, QWidget *parent,
                                                  const QString &caption,
                                                  const QString &dir,
                                                  const QString &filter);
  [[nodiscard]] static QString get_save_file_name(const QString &id, QWidget *parent,
                                                  const QString &caption,
                                                  const QString &dir,
                                                  const QString &filter);
  [[nodiscard]] static QString get_existing_directory(const QString &id,
                                                      QWidget *parent,
                                                      const QString &caption,
                                                      const QString &dir);

  // The pure half: where a dialog named `id` should open, given the caller's
  // explicit `dir`. Exposed so the rule is testable without a modal dialog.
  [[nodiscard]] static QString start_dir(const QString &id, const QString &dir);

  // Remember `path` as the last directory used by `id`.
  static void remember(const QString &id, const QString &path);

private:
  static QMap<QString, QString> cache_;
};

}  // namespace ui