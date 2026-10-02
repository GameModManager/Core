#pragma once

#include "engine/backup/backup_service.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <filesystem>
#include <functional>
#include <vector>

namespace ui {

class MainWindow;
class TaskDialog;

// Backup and restore points for an instance's load order and mod list
// (Workspace-czc0; MO2 mainwindow.cpp:3823-3940).
//
// Four MANUAL toolbar actions, two per tab, parked beside the "Active:"
// counter exactly where MO2 puts them (mainwindow.ui:840,863 and :326,340):
//
//   load order -> plugins.txt + loadorder.txt + lockedorder.txt  (MO2 :3845-3847)
//   mod list   -> modlist.txt                                    (MO2 :3922)
//
// Metadata only. NOT mod files, NOT archives.txt, NOT settings.ini. MO2's
// per-mod directory backup (ModListViewActions::createBackup,
// modlistviewactions.cpp:1041-1051) is a DIFFERENT feature - not timestamped,
// different trigger, different location - and is not this.
//
// There is NO automatic trigger. MO2 calls createBackup from exactly two
// toolbar slots and nowhere else; its Settings::keepBackupOnInstall() has no
// reader anywhere in the MO2 tree (design 1.8), so it is an orphan setting,
// not an auto-backup hook, and we do not implement it.
//
// Every byte of file handling belongs to engine::backup (the backup is a
// byte-for-byte copy, never a re-serialisation). This class owns the dialogs
// and the reload, and it carries our three deliberate divergences from MO2:
//   1. restore CONFIRMS before overwriting; MO2 overwrites silently (:3904-3906)
//   2. a safety backup of the live files is taken before the first write; MO2
//      has none at all, so a wrong restore there is unrecoverable. A file whose
//      safety copy FAILED is left alone rather than overwritten, and that is
//      reported per file - so no report can claim a blanket "nothing was
//      overwritten" over a set where some files did change
//   3. per-file failure reporting; MO2 ||-chains its three copies (:3904-3906),
//      so the first failure skips the rest and the profile is left silently mixed
// The view is reloaded after ANY file changed on disk, partial restores
// included: a mixed profile shown through a stale view is the one screen that
// is not telling the truth.
class BackupActions : public QObject {
  Q_OBJECT
public:
  explicit BackupActions(MainWindow *w, QObject *parent = nullptr);

  // The reloads live in ModListController (it owns the plugin database, the mod
  // model and the counter), so the controller hands them in. Both are called
  // AFTER a restore that changed something on disk.
  void set_reload_load_order(std::function<void()> cb) {
    reload_load_order_cb_ = std::move(cb);
  }
  void set_reload_mod_list(std::function<void()> cb) {
    reload_mod_list_cb_ = std::move(cb);
  }

public slots:
  // Write the stamped copies of plugins.txt + loadorder.txt + lockedorder.txt.
  void create_load_order_backup();
  // Pick a load-order backup, confirm, restore, reload the Plugins tab.
  void restore_load_order_backup();
  // Write the stamped copy of modlist.txt.
  void create_mod_list_backup();
  // Pick a mod-list backup, confirm, restore, reload the mod list.
  void restore_mod_list_backup();

private:
  // The live files a kind covers, read off the active ProfileManager. Empty
  // when no instance is loaded - every action is then a no-op.
  [[nodiscard]] std::vector<std::filesystem::path>
  files_for(engine::backup::BackupKind kind) const;

  // The picker. Returns the chosen RAW suffix, or empty when the user
  // cancelled. `no_candidates` is set when the picker had nothing to show, so
  // the caller can say so the way MO2 does ("There are no backups to restore",
  // mainwindow.cpp:3885-3886) instead of silently doing nothing.
  [[nodiscard]] QString pick_stamp(const std::filesystem::path &anchor,
                                   const QString &title, bool *no_candidates);

  // One shared restore path: confirm, restore, report, reload.
  void run_restore(engine::backup::BackupKind kind, const QString &what);

  // Report a backup: transient success, or one error line per failed file.
  void report(const engine::backup::BackupResult &result, const QString &what);
  void report(const engine::backup::RestoreResult &result);

  void transient(const QString &text) const;

  MainWindow *w_ = nullptr;
  std::function<void()> reload_load_order_cb_;
  std::function<void()> reload_mod_list_cb_;
};

// Testable seam, the same shape as configure_remove_mods_dialog
// (mod_actions.h:19): fills a TaskDialog with the restore confirmation, naming
// the files that are about to be replaced. Shared between this class and the
// dialog tests.
void configure_restore_backup_dialog(TaskDialog &dlg, const QString &what,
                                     const QStringList &file_names,
                                     const QString &stamp);

}  // namespace ui
