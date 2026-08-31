#pragma once

#include <QJsonObject>
#include <QStringList>
#include <QWidget>
#include <filesystem>

#include "ui/widgets/executables_entry.h"

class QComboBox;
class QShortcut;
class QToolButton;

namespace ui {

// Sentinel item text shown in the executable combo (the "add new entry" slot).
// Kept in the header so main_window.cpp can compare against it without relying
// on a translated string. Display sites wrap it in tr().
inline constexpr const char *kAddNewEntryText = "<Edit...>";

// Single exe-icon extraction path for the executables combo, the toolbar
// shortcuts and Data-tab "Add as Executable": cached <basename>.ico in
// icon_cache_dir wins, then wrestool extraction (cached on success), then
// QFileIconProvider, then the standard file icon. Never returns a null icon.
QIcon extractExeIcon(const QString &exePath,
                     const std::filesystem::path &icon_cache_dir);

class ExecControlsBar : public QWidget {
  Q_OBJECT
public:
  explicit ExecControlsBar(QWidget *parent = nullptr);

  void set_executables(const QStringList &names,
                       const QString &default_name = {},
                       const std::filesystem::path &game_dir = {},
                       const std::filesystem::path &icon_cache_dir = {},
                       const std::filesystem::path &staging_dir = {});
  void clear_executables();
  [[nodiscard]] QString current_executable() const;
  [[nodiscard]] int current_executable_index() const;
  [[nodiscard]] QStringList executable_paths() const;

  // Returns the full Executables::Entry for each item (excluding the sentinel)
  [[nodiscard]] QVector<Executables::Entry> executable_entries() const;

  // Returns the Executables::Entry for the currently selected item
  [[nodiscard]] Executables::Entry current_entry() const;

  // Legacy: adds an entry with just a path (no extra metadata)
  void add_executable(const QString &display_name, const QString &rel_path,
                      const QIcon &icon = {});

  // New: adds a full entry with metadata
  void add_entry(const Executables::Entry &entry);

  // Restores the combo selection to the entry whose path matches. Returns
  // false if no such entry exists. Fires current_executable_changed() so the
  // in-memory selection tracker follows.
  bool select_executable(const QString &path);

signals:
  void run_clicked();
  void shortcut_to_toolbar();
  void shortcut_to_desktop();
  void add_entry_requested();
  void current_executable_changed();

private:
  QJsonObject item_data(int index) const;
  void set_item_data(int index, const QJsonObject &obj);

  QComboBox *exec_combo_ = nullptr;
  QToolButton *run_btn_ = nullptr;
  QToolButton *shortcut_btn_ = nullptr;
  QShortcut *run_shortcut_ = nullptr;
  QShortcut *shortcut_menu_shortcut_ = nullptr;

  // Index of the last real (non-sentinel) selection, used to restore the
  // combo when the user picks "<Edit...>" instead of jumping to the first
  // real entry.
  int last_real_index_ = 1;

  // Resolution context for combo item icons. Fed by set_executables; used by
  // add_entry so both rebuild paths resolve icons identically (custom icon
  // first, then the cached wrestool/QFileIconProvider extraction).
  std::filesystem::path game_dir_;
  std::filesystem::path icon_cache_dir_;
  // Deploy staging dir (".gmm_staging") for merged-view icon fallback:
  // mod-provided executables exist there after deploy but not at
  // game_dir/path physically.
  std::filesystem::path staging_dir_;
};

} // namespace ui
