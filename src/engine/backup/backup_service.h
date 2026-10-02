#pragma once

#include "engine/profile/profile.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// engine/backup - timestamped backup and restore points for a profile's load
// order and mod list (Workspace-czc0).
//
// This is MO2's MainWindow::createBackup / queryRestore / on_restoreButton
// feature (references/modorganizer/src/mainwindow.cpp:3823-3940), and the one
// thing to understand about it is that MO2's backup is a BYTE-FOR-BYTE COPY of
// the live file, not a re-serialisation: it builds a stamped destination name
// and shellCopy()s the file (mainwindow.cpp:3830-3831). Everything that would
// normally parse and re-emit a profile file - parse_modlist, serialize_modlist,
// read/write_locked_order, read_lines/write_lines in instance_snapshot.cpp - is
// therefore BYPASSED. A backup is: flush, stamp, copy the bytes. A restore is:
// read the chosen backup's bytes, safe_write_file them over the live file.
//
// BOUNDARY: profile metadata only, four files. plugins.txt, loadorder.txt,
// lockedorder.txt (one "load order" action) and modlist.txt (one "mod list"
// action). NOT mod files, NOT archives.txt, NOT settings.ini. MO2's per-mod
// directory backup (ModListViewActions::createBackup, modlistviewactions.cpp:
// 1041-1051) is a DIFFERENT feature - not timestamped, different trigger,
// different location - and is not modelled here.
//
// NOT this, either: the deploy Original_Files store (deploy_utils.h:153-159).
// Same word, different directory, different lifecycle. If you are merging that
// with this file, you are about to break both.
//
// Backups land NEXT TO the file they copy, inside the profile directory, named
// `<live file name including its extension>.<yyyy_MM_dd_hh_mm_ss>` in LOCAL
// time (mainwindow.cpp:3830). A user who already ran MO2 sees their existing
// timestamps listed here with no migration.
//
// Qt-free: the engine owns the file handling, the UI owns every dialog. Three
// deliberate divergences from MO2, all on the restore path:
//   1. the UI confirms before overwriting (MO2 overwrites silently, :3904-3906)
//   2. restore_backup takes an UNCONDITIONAL pre-restore safety backup
//      (MO2 has none, so a bad restore there is unrecoverable)
//   3. restore_backup attempts and reports every file independently (MO2
//      ||-chains its three copies, so the first failure skips the rest and
//      leaves the profile silently mixed)
// plus eviction ordered by the PARSED stamp rather than by file-name sort.
namespace engine::backup {

// Which metadata set one user action covers.
enum class BackupKind {
  LoadOrder,  // plugins.txt + loadorder.txt + lockedorder.txt (MO2 :3845-3847)
  ModList,    // modlist.txt (MO2 :3922)
};

// Copies kept per source file. MO2 hardcodes 10 at mainwindow.cpp:3834; it is
// not a setting, has no dialog entry, and is not configurable here on purpose.
inline constexpr std::size_t kKeepCount = 10;

// One restore candidate: a regular file next to `anchor` whose name is
// `<anchor file name>.<suffix>`.
struct BackupEntry {
  std::string suffix;  // raw text after the anchor's file name and the '.'
  std::filesystem::path path;
  // A well-formed yyyy_MM_dd_hh_mm_ss (optionally plus our same-second
  // `-N` suffix). MO2's three-way classification (mainwindow.cpp:3860-3881).
  bool is_stamp = false;
  // An orphaned atomic-write temporary left by a crash or power loss. Offered,
  // but warned about - MO2 :3874-3878. Two shapes count: MO2's six random
  // letters, and OUR OWN temp form, which safe_write_file names
  // `<target>.tmp<pid>_<counter>` (safe_write_file.cpp:64). Warning only about
  // MO2's shape would never fire for a temp this app produced.
  bool is_orphan_temp = false;
  // The parsed calendar key; 0 for a suffix that is not a stamp.
  std::int64_t stamp_key = 0;
  // The same-second disambiguator's N (0 when there is none). Two copies of one
  // second share a stamp_key and are ordered by this - numerically, because a
  // name sort puts "...-10" before "...-2" and would evict the newest of them.
  int copy_index = 0;
};

// Why one file did not back up / did not restore. Every value leaves that ONE
// file alone; the sibling files in the set are always still attempted, and the
// reason is carried per file so a partial operation is never silent.
enum class RestoreFailure {
  None,
  // The caller-supplied flush failed: nothing was written, nothing is copied.
  // Copying the pre-flush bytes and calling it a backup is worse than no
  // feature at all (design 6.6).
  FlushFailed,
  // No such live file.
  MissingLiveFile,
  // No backup of this file at the chosen stamp. The other files still restore.
  NoSuchBackup,
  // A backup exists at that stamp but its bytes could not be read.
  UnreadableBackup,
  // The profile directory or the live file denies writes.
  NotWritable,
  // The write itself failed (out of space, a failed rename, ...).
  WriteFailed,
  // The pre-restore safety copy of THIS file failed, so the file was left
  // alone rather than overwritten with no copy of what it held. The whole
  // point of the safety backup is that a wrong restore stays recoverable;
  // overwriting a file whose copy failed would throw that away. Per file, so
  // its siblings still restore and the report still names every one.
  SafetyBackupFailed,
};

// One file's outcome. `file` is the file NAME (what the report says), never a
// full path: the user knows the profile, not the directory layout.
struct FileResult {
  std::string file;
  bool ok                = false;
  RestoreFailure failure = RestoreFailure::None;
  std::string detail;  // the errno/system text, for the report
};

struct BackupResult {
  // The stamp every copy shares (local time, `yyyy_MM_dd_hh_mm_ss`, plus the
  // `-N` disambiguator when the same second already had one).
  std::string stamp;
  std::vector<FileResult> files;

  [[nodiscard]] bool ok() const;
};

struct RestoreResult {
  std::vector<FileResult> files;
  // Stamp of the pre-restore safety backup, taken before the first write.
  // Empty when no copy was made at all.
  std::string safety_stamp;
  // True when the safety copy succeeded for EVERY file. Per-file truth lives in
  // files[] (RestoreFailure::SafetyBackupFailed), not here: a set-wide flag
  // cannot describe a set where one file's copy failed and another's did not,
  // and a UI that turns it into "nothing was overwritten" would then be lying
  // about the files that DID restore.
  bool safety_ok = false;

  [[nodiscard]] bool ok() const;
  // True when at least one file was restored. A partial restore still needs the
  // view reloaded: the profile is mixed, and the screen showing the old state is
  // the one thing not telling the truth.
  [[nodiscard]] bool any_restored() const;
};

// The live files a backup of `kind` covers, in copy order. Reads the paths off
// ProfileManager so the four filenames live in exactly one place.
[[nodiscard]] std::vector<std::filesystem::path>
backup_files(BackupKind kind, const profile::ProfileManager &profile);

// The flush, run before the copies so the live files are CURRENT rather than
// whatever the debounced writer last happened to emit (MO2 flushes first too:
// savePluginList at :3843, writeModlistNow at :3920). The caller supplies it
// because the two kinds have different owners - PluginDb::Database::save_profile
// for the plugin files, ProfileManager::write_modlist_now for modlist.txt.
// Returning false aborts the whole backup.
//
// Note on what "failed" can mean today: neither production writer returns a
// status (save_profile and write_modlist_now are both void), so a caller's
// honest answer is "the writer ran". What IS checked here is the consequence
// that matters: copy_file's result per file, and the live file's existence, so
// a backup that copied nothing cannot report success.
using FlushFn = std::function<bool()>;

// Create the stamped copies of `files`, then sweep the retention rule per file.
// Every copy_file result is checked: a backup path that ignores its own result
// reports success while having done nothing.
[[nodiscard]] BackupResult
create_backup(const std::vector<std::filesystem::path> &files, const FlushFn &flush,
              std::chrono::system_clock::time_point now);

// The files of a profile, resolved from its ProfileManager.
[[nodiscard]] BackupResult create_backup(BackupKind kind,
                                         const profile::ProfileManager &profile,
                                         const FlushFn &flush,
                                         std::chrono::system_clock::time_point now);

// Enumerate the restore candidates for `anchor`, newest first. Regular files
// only, anchored on the anchor's own name and directory, so a candidate can
// only ever be a sibling of the current file (MO2 :3855).
[[nodiscard]] std::vector<BackupEntry>
list_backups(const std::filesystem::path &anchor);

// Restore `stamp` into every file of `files`.
//
// Order is load-bearing:
//   1. read every source up front, so nothing later can remove the bytes we are
//      about to write (the safety backup's retention sweep could otherwise
//      evict the very backup being restored);
//   2. take a safety backup of the live files - attempted whatever happens next,
//      because the live state is what a wrong restore destroys; no flush here,
//      the point is to capture the on-disk bytes as they are;
//   3. write each file independently and report every failure by name.
//
// A file whose safety copy FAILED is not overwritten: writing it would destroy
// a state there is no copy of, which is the one outcome the safety backup
// exists to prevent. Its siblings still restore. Both facts are reported per
// file, so no report can claim a blanket "nothing was overwritten" over a set
// where some files did change.
[[nodiscard]] RestoreResult
restore_backup(const std::vector<std::filesystem::path> &files,
               const std::string &stamp, std::chrono::system_clock::time_point now);

// True when `suffix` is a backup stamp: 19 characters in yyyy_MM_dd_hh_mm_ss
// with every field in range, optionally followed by the `-N` same-second
// disambiguator create_backup appends. Anything else is a hand-made copy, and
// is never a candidate for eviction - MO2's sweep uses a loose '?'-glob
// (mainwindow.cpp:3823) and will delete a copy that is not a backup at all.
[[nodiscard]] bool is_backup_stamp(const std::string &suffix);

// The key a stamp sorts by, or 0 when it is not a stamp. Monotonic in the
// wall-clock fields and free of any timezone or DST interpretation: the stamp
// is LOCAL time, so a DST change or a backwards clock jump must not reorder the
// history - which is exactly what MO2's QDir::Name ordering cannot promise
// (design 6.4).
[[nodiscard]] std::int64_t stamp_key(const std::string &suffix);

// The same-second disambiguator's N (0 when the suffix carries none). Two
// copies of one second share a stamp_key, so this is what tells them apart.
[[nodiscard]] int stamp_copy_index(const std::string &suffix);

// `<live file name>.<stamp>` - the destination name of one backup copy.
[[nodiscard]] std::filesystem::path backup_path_for(const std::filesystem::path &live,
                                                    const std::string &stamp);

}  // namespace engine::backup