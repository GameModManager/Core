#include "ui/controllers/backup_actions.h"

#include "ui/main_window/main_window.h"
#include "ui/widgets/error_popup.h"
#include "ui/widgets/list_dialog.h"
#include "ui/widgets/status_bar.h"
#include "ui/widgets/task_dialog.h"

#include <QDateTime>
#include <QDialog>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QVariant>

#include <chrono>

namespace ui {

namespace {

  // Human-readable label for a candidate's raw suffix (MO2 :3871-3873 gives a
  // localised timestamp for a real stamp and the raw suffix for anything else).
  // A plain stamp reads as a time; a same-second copy keeps its -N so two
  // backups taken in one second are told apart; a six-letter suffix is an
  // orphaned atomic-write temporary from a crash and says so, which is the
  // warning MO2 puts in the picker's description column (mainwindow.cpp:
  // 3874-3878). ListDialog has no per-item description, so the warning rides on
  // the label instead - a Phase-3 follow-up moves it to a real description.
  QString choice_label(const engine::backup::BackupEntry &entry) {
    const QString suffix = QString::fromStdString(entry.suffix);
    if (entry.is_orphan_temp)
      return QObject::tr("%1 - might be left over from a crash or power loss; "
                         "check it before restoring")
          .arg(suffix);
    if (!entry.is_stamp)
      return suffix;

    // The stamp is LOCAL time and the format carries no zone specifier, so
    // fromString hands back a Qt::LocalTime QDateTime already - there is
    // nothing to reinterpret, and the deprecated setTimeSpec() would only
    // churn.
    QDateTime parsed =
        QDateTime::fromString(suffix.left(19), QStringLiteral("yyyy_MM_dd_hh_mm_ss"));
    if (!parsed.isValid())
      return suffix;
    const QString label = QLocale().toString(parsed, QLocale::ShortFormat);

    // A same-second copy keeps its -N, so two backups taken in one second are
    // told apart instead of showing an identical timestamp twice.
    const int disambiguator = suffix.indexOf('-', 19);
    if (disambiguator < 0)
      return label;
    return QObject::tr("%1 (#%2)").arg(label, suffix.mid(disambiguator + 1));
  }

  // One line per file that did not make it, naming the file. Never a single
  // generic message: after a partial restore the profile is mixed and the user
  // has to know which half is stale.
  QString describe(const engine::backup::FileResult &file) {
    using engine::backup::RestoreFailure;
    const QString name   = QString::fromStdString(file.file);
    const QString detail = QString::fromStdString(file.detail);
    switch (file.failure) {
    case RestoreFailure::None:
      return {};
    case RestoreFailure::FlushFailed:
      return QObject::tr("The %1 could not be written, so nothing was backed up.")
          .arg(name);
    case RestoreFailure::MissingLiveFile:
      return QObject::tr("There is no %1 to back up.").arg(name);
    case RestoreFailure::SafetyBackupFailed:
      // The engine leaves this file alone rather than overwrite a state it has
      // no copy of. Saying so is the point: the user must know this one was
      // SPARED, not that it was skipped.
      return detail.isEmpty()
                 ? QObject::tr("%1 could not be backed up before restoring, so it "
                               "was left unchanged.")
                       .arg(name)
                 : QObject::tr("%1 could not be backed up before restoring (%2), so "
                               "it was left unchanged.")
                       .arg(name, detail);
    case RestoreFailure::NoSuchBackup:
      return QObject::tr("No backup of %1 at %2.").arg(name, detail);
    case RestoreFailure::UnreadableBackup:
      return QObject::tr("Could not read the backup of %1.").arg(name);
    case RestoreFailure::NotWritable:
      return QObject::tr("Could not write %1: %2").arg(name, detail);
    case RestoreFailure::WriteFailed:
      return QObject::tr("Could not write %1: %2").arg(name, detail);
    }
    return {};
  }

  QStringList failure_lines(const std::vector<engine::backup::FileResult> &files) {
    QStringList lines;
    for (const auto &file : files) {
      if (file.ok)
        continue;
      lines << describe(file);
    }
    return lines;
  }

}  // namespace

void configure_restore_backup_dialog(TaskDialog &dlg, const QString &what,
                                     const QStringList &file_names,
                                     const QString &stamp) {
  dlg.title(QObject::tr("Restore Backup"))
      .main(QObject::tr("Replace the current %1 with the backup from %2?")
                .arg(what, stamp))
      .content(QObject::tr("The current %1 will be backed up first, so this can be "
                           "undone from the same list.")
                   .arg(what))
      .details(file_names.join("\n"))
      .icon(QMessageBox::Warning)
      .add_button({QObject::tr("Restore"), "", QMessageBox::Yes})
      .add_button({QObject::tr("Cancel"), "", QMessageBox::No});
}

BackupActions::BackupActions(MainWindow *w, QObject *parent) : QObject(parent), w_(w) {}

std::vector<std::filesystem::path>
BackupActions::files_for(engine::backup::BackupKind kind) const {
  if (!w_ || !w_->active_profile_)
    return {};
  return engine::backup::backup_files(kind, *w_->active_profile_);
}

void BackupActions::transient(const QString &text) const {
  if (w_ && w_->status_bar_)
    w_->status_bar_->set_status(text);
}

QString BackupActions::pick_stamp(const std::filesystem::path &anchor,
                                  const QString &title, bool *no_candidates) {
  // Anchored on the LIVE file: the picker can only ever offer siblings of the
  // current file, so cross-profile restore is structurally impossible here
  // (MO2 :3855 anchors the same way).
  const auto entries = engine::backup::list_backups(anchor);
  if (no_candidates != nullptr)
    *no_candidates = entries.empty();
  if (entries.empty())
    return {};

  QStringList choices;
  QList<QVariant> payloads;
  choices.reserve(static_cast<int>(entries.size()));
  for (const auto &entry : entries) {
    choices << choice_label(entry);
    // The RAW suffix travels as the payload, so the restore path never has to
    // re-parse a display string (MO2 :3872-3873 does the same).
    payloads << QString::fromStdString(entry.suffix);
  }

  ListDialog dlg(w_);
  dlg.setWindowTitle(title);
  dlg.setChoices(choices);
  dlg.setChoiceData(payloads);
  dlg.setCurrentRow(0);
  if (dlg.exec() != QDialog::Accepted)
    return {};
  return dlg.getChoiceData().toString();
}

void BackupActions::create_load_order_backup() {
  const auto files = files_for(engine::backup::BackupKind::LoadOrder);
  if (files.empty())
    return;
  // Flush FIRST so the copy is of current state and not of whatever the last
  // save wrote (MO2 flushes too: savePluginList :3843). Database::save_profile
  // is synchronous, so this does not reintroduce MO2's deferred-flush hazard
  // where a backup copies stale files (design 6.3).
  const auto profiles_dir = w_->profiles_dir_path();
  const auto profile_name = w_->current_profile_name_;
  const auto result       = engine::backup::create_backup(
      files,
      [this, &profiles_dir, &profile_name] {
        if (profiles_dir.empty())
          return false;
        w_->plugins_db_.save_profile(profiles_dir, profile_name);
        return true;
      },
      std::chrono::system_clock::now());
  report(result, tr("load order"));
}

void BackupActions::create_mod_list_backup() {
  const auto files = files_for(engine::backup::BackupKind::ModList);
  if (files.empty())
    return;
  // Flush the debounced modlist writer first (MO2 writeModlistNow :3920), so
  // the copy is of current state and not of the last thing the ~5s writer
  // happened to emit.
  const auto result = engine::backup::create_backup(
      files,
      [this] {
        w_->active_profile_->write_modlist_now();
        return true;
      },
      std::chrono::system_clock::now());
  report(result, tr("mod list"));
}

void BackupActions::restore_load_order_backup() {
  run_restore(engine::backup::BackupKind::LoadOrder, tr("load order"));
}

void BackupActions::restore_mod_list_backup() {
  run_restore(engine::backup::BackupKind::ModList, tr("mod list"));
}

void BackupActions::run_restore(engine::backup::BackupKind kind, const QString &what) {
  const auto files = files_for(kind);
  if (files.empty())
    return;

  bool no_candidates = false;
  const QString stamp =
      pick_stamp(files.front(), tr("Choose backup to restore"), &no_candidates);
  if (no_candidates) {
    QMessageBox::information(w_, tr("No Backups"),
                             tr("There are no backups to restore"));
    return;
  }
  // Cancelled: a silent no-op, exactly as MO2 (:3893 returns empty and the
  // handler does nothing at all).
  if (stamp.isEmpty())
    return;

  QStringList file_names;
  file_names.reserve(static_cast<int>(files.size()));
  for (const auto &file : files)
    file_names << QString::fromStdString(file.filename().string());

  // Divergence 1: MO2 overwrites the live files with no prompt at all
  // (mainwindow.cpp:3904-3906). A load order is the difference between a game
  // that starts and one that does not.
  TaskDialog confirm(w_, tr("Restore Backup"));
  configure_restore_backup_dialog(confirm, what, file_names, stamp);
  if (confirm.exec() != QMessageBox::Yes)
    return;

  // The mod-list restore is the one with a stale in-memory twin: ProfileManager
  // debounces modlist.txt writes by 5s, so a pending write would overwrite the
  // file we just restored. Cancel it - and do NOT flush, which is the mistake
  // MO2's refresh(true) makes (organizercore.cpp:1281-1283, design 6.2).
  if (kind == engine::backup::BackupKind::ModList && w_->active_profile_)
    w_->active_profile_->cancel_modlist_write();

  // Divergence 2 and 3 both live in the engine call: it takes a safety backup
  // before the first write (and refuses to overwrite a file whose safety copy
  // failed), and reports every file separately.
  const auto result = engine::backup::restore_backup(files, stamp.toStdString(),
                                                     std::chrono::system_clock::now());
  report(result);

  // Reload whenever ANY file changed on disk - not only when all of them did.
  // A partial restore leaves the profile MIXED by design (divergence 3), and
  // skipping the reload there would show the user a view that agrees with two
  // thirds of the disk while the report tells them the third is stale.
  if (!result.any_restored())
    return;
  if (kind == engine::backup::BackupKind::LoadOrder) {
    if (reload_load_order_cb_)
      reload_load_order_cb_();
  } else if (reload_mod_list_cb_) {
    reload_mod_list_cb_();
  }
}

void BackupActions::report(const engine::backup::BackupResult &result,
                           const QString &what) {
  if (result.ok()) {
    transient(tr("Backup of %1 created").arg(what));
    return;
  }
  ui::report_error(tr("Could not create the backup of the %1.").arg(what), w_,
                   failure_lines(result.files).join("\n"));
}

void BackupActions::report(const engine::backup::RestoreResult &result) {
  if (result.ok()) {
    transient(tr("Backup restored"));
    return;
  }
  // NEVER a blanket claim about the whole set. The engine refuses to overwrite
  // a file whose safety copy failed, so some files may well have changed while
  // others did not; a single "nothing was overwritten" over that state is a
  // lie, and dropping failure_lines() here would also lose the per-file
  // reporting that divergence 3 exists for. The headline says what is true for
  // every combination, and the detail names every file that did not change.
  const bool none_restored = !result.any_restored();
  const QString headline =
      none_restored ? tr("The backup could not be restored. Nothing was changed.")
                    : tr("The backup was only partly restored. The files listed below "
                         "were not changed, so the profile is now in a mixed state.");
  ui::report_error(headline, w_, failure_lines(result.files).join("\n"));
}

}  // namespace ui
