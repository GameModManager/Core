#include "engine/backup/backup_service.h"

#include "engine/profile/safe_write_file.h"
#include "platform/platform.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace engine::backup {

namespace {

  // MO2's PATTERN_BACKUP_DATE (mainwindow.cpp:3826). Local time, zero padded.
  constexpr std::size_t kStampLength = 19;  // yyyy_MM_dd_hh_mm_ss
  // Ceiling on the `-N` disambiguator's digits: `copy` is an int, so 9 is the
  // largest count that cannot overflow it.
  constexpr std::size_t kMaxCopyDigits = 9;

  std::tm local_calendar(std::chrono::system_clock::time_point tp) {
    return engine::local_time(std::chrono::system_clock::to_time_t(tp));
  }

  std::string format_stamp(std::chrono::system_clock::time_point tp) {
    const auto tm = local_calendar(tp);
    std::string out;
    // Appended field by field rather than through one snprintf: a format string
    // cannot prove its own output fits the buffer, and the compiler is right to
    // warn about it.
    auto append_padded = [&out](int value, std::size_t width) {
      const std::string text = std::to_string(value);
      if (text.size() < width)
        out.append(width - text.size(), '0');
      out += text;
    };
    append_padded(tm.tm_year + 1900, 4);
    for (const int value :
         {tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec}) {
      out.push_back('_');
      append_padded(value, 2);
    }
    return out;
  }

  // Exactly `count` digits from `at`. A hand-rolled check beats <cctype> here:
  // isdigit() is UB on a negative char and locale-dependent.
  bool all_digits(std::string_view text, std::size_t at, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
      const char c = text[at + i];
      if (c < '0' || c > '9')
        return false;
    }
    return true;
  }

  int field(std::string_view text, std::size_t at, std::size_t count) {
    int value = 0;
    for (std::size_t i = 0; i < count; ++i)
      value = value * 10 + (text[at + i] - '0');
    return value;
  }

  // Six ASCII letters and nothing else. MO2's regex is `\.([A-Za-z]{6})`
  // (mainwindow.cpp:3862): it identifies an orphaned atomic-write temporary
  // left by a crash, which is not a backup and must be labelled as such.
  bool is_six_letters(const std::string &text) {
    if (text.size() != 6)
      return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    });
  }

  // OUR OWN atomic-write temporary: safe_write_file names its temp
  // `<target>.tmp<pid>_<counter>` (safe_write_file.cpp:64), so the suffix is
  // `tmp<pid>_<n>`. MO2's six-letter test never matches it, which would leave a
  // half-written plugins.txt - exactly the bad backup design 6.1 calls the
  // realistic one - listed with no warning at all.
  bool is_our_atomic_temp(const std::string &text) {
    const std::string prefix = "tmp";
    if (text.size() <= prefix.size() + 2 || text.compare(0, prefix.size(), prefix) != 0)
      return false;
    const std::size_t underscore = text.find('_', prefix.size());
    if (underscore == std::string::npos || underscore == prefix.size() ||
        underscore + 1 == text.size())
      return false;
    return all_digits(text, prefix.size(), underscore - prefix.size()) &&
           all_digits(text, underscore + 1, text.size() - underscore - 1);
  }

  struct SourceRead {
    bool exists   = false;
    bool readable = false;
    std::string bytes;
  };

  SourceRead read_source(const std::filesystem::path &source) {
    SourceRead read;
    std::error_code ec;
    read.exists = std::filesystem::is_regular_file(source, ec);
    if (!read.exists)
      return read;
    std::ifstream in(source, std::ios::binary);
    if (!in)
      return read;
    read.bytes.assign(std::istreambuf_iterator<char>(in),
                      std::istreambuf_iterator<char>());
    // An empty backup is legal; a stream that died mid-read is not.
    read.readable = in.good() || in.eof();
    return read;
  }

  // Can `file` be opened for writing WITHOUT creating it? `std::ofstream` with
  // any mode creates a missing file, which on the error path would leave a
  // 0-byte live file behind - and an empty plugins.txt reads as a perfectly
  // valid load order of zero plugins. fstream(in|out) opens only an existing
  // file and touches nothing.
  //
  // This replaces a permissions-mode pre-check on the PARENT directory, which
  // was both wrong (it refused a writable directory whose owner bit was clear -
  // ntfs-3g, exFAT, CIFS - and accepted a read-only file in a writable one) and
  // redundant: safe_write_file already returns a bool and leaves the target
  // untouched when it fails.
  bool file_is_openable(const std::filesystem::path &file) {
    std::fstream probe(file, std::ios::in | std::ios::out);
    return probe.is_open();
  }

  // Every `<live file name>.<suffix>` regular sibling of `live`.
  std::vector<BackupEntry> siblings_of(const std::filesystem::path &live) {
    std::vector<BackupEntry> entries;
    const auto directory = live.parent_path();
    if (directory.empty())
      return entries;

    const std::string prefix = live.filename().string() + ".";
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
      if (ec)
        break;
      std::error_code file_ec;
      // Regular files only: a directory cannot be a backup.
      if (!entry.is_regular_file(file_ec))
        continue;
      const auto name = entry.path().filename().string();
      if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0)
        continue;
      BackupEntry be;
      be.suffix         = name.substr(prefix.size());
      be.path           = entry.path();
      be.stamp_key      = stamp_key(be.suffix);
      be.copy_index     = stamp_copy_index(be.suffix);
      be.is_stamp       = be.stamp_key != 0;
      be.is_orphan_temp = is_six_letters(be.suffix) || is_our_atomic_temp(be.suffix);
      entries.push_back(std::move(be));
    }
    return entries;
  }

  // Keep the `kKeepCount` newest stamped backups of `live`; evict the rest.
  //
  // Entries whose suffix must never be evicted. Two reasons to be on the list:
  //   - the stamp just created, because a backwards clock jump (or a DST
  //     fall-back that repeats a local hour) makes it the OLDEST by stamp, and
  //     evicting it would delete the newest thing the user did - the exact
  //     outcome the rule exists to prevent;
  //   - on the restore path, the stamp being restored, so the backup the user
  //     picked cannot be pushed out of history by the safety copy that restore
  //     takes. It vanishes from the picker afterwards if it does.
  // A protected entry in the eviction set is skipped and the next-oldest goes.
  void sweep(const std::filesystem::path &live,
             const std::vector<std::string> &protected_stamps) {
    std::vector<BackupEntry> stamped;
    for (auto &entry : siblings_of(live)) {
      if (entry.is_stamp)
        stamped.push_back(std::move(entry));
    }
    if (stamped.size() <= kKeepCount)
      return;

    // Oldest first. Two copies of the SAME second share a stamp_key, so the
    // copy index breaks the tie numerically: a name sort puts "...-10" before
    // "...-2" and would evict the newest of them.
    std::stable_sort(stamped.begin(), stamped.end(),
                     [](const BackupEntry &a, const BackupEntry &b) {
                       if (a.stamp_key != b.stamp_key)
                         return a.stamp_key < b.stamp_key;
                       return a.copy_index < b.copy_index;
                     });

    std::size_t to_drop = stamped.size() - kKeepCount;
    for (const auto &entry : stamped) {
      if (to_drop == 0)
        return;
      if (std::find(protected_stamps.begin(), protected_stamps.end(), entry.suffix) !=
          protected_stamps.end())
        continue;
      std::error_code ec;
      std::filesystem::remove(entry.path, ec);
      --to_drop;
    }
  }

  // The first unused `<file>.<stamp>`, appending `-2`, `-3`, ... while the
  // plain stamp is taken. MO2 has no such check, so a same-second double click
  // silently overwrites the first backup (design 6.7).
  std::string unique_stamp(const std::vector<std::filesystem::path> &files,
                           std::chrono::system_clock::time_point now) {
    const std::string base = format_stamp(now);
    auto taken             = [&](const std::string &candidate) {
      std::error_code ec;
      return std::any_of(
          files.begin(), files.end(), [&](const std::filesystem::path &f) {
            return std::filesystem::exists(backup_path_for(f, candidate), ec);
          });
    };
    std::string stamp = base;
    for (int n = 2; taken(stamp); ++n)
      stamp = base + "-" + std::to_string(n);
    return stamp;
  }

  // Copy every file to `<file>.<stamp>`, then sweep each file's own history.
  // No flush: the caller decides whether the live files need making current
  // first (that is what separates a backup from the safety copy taken on the
  // restore path). `extra_protected` holds any further suffix the sweep must
  // not evict - the restore path passes the stamp being restored.
  BackupResult copy_with_stamp(const std::vector<std::filesystem::path> &files,
                               const std::string &stamp,
                               const std::vector<std::string> &extra_protected = {}) {
    BackupResult result;
    result.stamp = stamp;

    // The stamp just created is always protected from the sweep; the restore
    // path adds the stamp it is restoring, so the safety copy cannot push the
    // backup the user picked out of the history they are looking at.
    std::vector<std::string> protected_stamps;
    protected_stamps.reserve(extra_protected.size() + 1);
    protected_stamps.push_back(stamp);
    protected_stamps.insert(protected_stamps.end(), extra_protected.begin(),
                            extra_protected.end());

    for (const auto &file : files) {
      FileResult fr;
      fr.file = file.filename().string();
      std::error_code ec;
      if (!std::filesystem::is_regular_file(file, ec)) {
        fr.failure = RestoreFailure::MissingLiveFile;
        fr.detail  = "the file does not exist";
        result.files.push_back(std::move(fr));
        continue;
      }
      // copy_file's result IS the feature: ignoring it is how a backup reports
      // success while having done nothing (design 6.6).
      if (!std::filesystem::copy_file(file, backup_path_for(file, stamp),
                                      std::filesystem::copy_options::overwrite_existing,
                                      ec)) {
        fr.failure = RestoreFailure::WriteFailed;
        fr.detail  = ec.message();
        result.files.push_back(std::move(fr));
        // No sweep on a failed copy: nothing was added to this file's history,
        // and evicting now would make room for nothing while quietly dropping
        // an existing backup the user may still want.
        continue;
      }
      fr.ok = true;
      result.files.push_back(std::move(fr));
      // Swept per source file, so a failure on one never takes another's
      // history with it (MO2 :3833-3834 also sweeps once per source file), and
      // only after the copy landed.
      sweep(file, protected_stamps);
    }
    return result;
  }

  // The calendar key. Monotonic in the wall-clock fields (years, then months,
  // days, hours, minutes, seconds) and nothing else: no timezone offset and no
  // DST folding, so two stamps compare the way their names read. That is the
  // point of parsing instead of string-comparing - MO2's QDir::Name order only
  // works because the stamp happens to be fixed-width (design 6.4).
  constexpr std::int64_t calendar_key(int year, int month, int day, int hour,
                                      int minute, int second) {
    return static_cast<std::int64_t>(year) * 100000000LL +
           static_cast<std::int64_t>(month) * 1000000LL +
           static_cast<std::int64_t>(day) * 10000LL +
           static_cast<std::int64_t>(hour) * 100LL +
           static_cast<std::int64_t>(minute) * 100LL +
           static_cast<std::int64_t>(second);
  }

  // Split a suffix into its calendar key and its same-second disambiguator. The
  // key is 0 and the copy index 0 when the suffix is not a stamp at all.
  void parse_stamp(const std::string &suffix, std::int64_t *key, int *copy) {
    *key  = 0;
    *copy = 0;
    if (suffix.size() < kStampLength)
      return;

    // Fixed shape first: `yyyy_MM_dd_hh_mm_ss`, so the separators sit at
    // indices 4, 7, 10, 13 and 16 and every other character of the 19 is a
    // digit.
    if (suffix[4] != '_' || suffix[7] != '_' || suffix[10] != '_' ||
        suffix[13] != '_' || suffix[16] != '_')
      return;
    if (!all_digits(suffix, 0, 4) || !all_digits(suffix, 5, 2) ||
        !all_digits(suffix, 8, 2) || !all_digits(suffix, 11, 2) ||
        !all_digits(suffix, 14, 2) || !all_digits(suffix, 17, 2))
      return;

    // Range check: `9999_99_99_99_99_99` has the right shape and is not a moment
    // in time. MO2's strict regex accepts it; we do not, because a name that is
    // not a stamp must never be a candidate for eviction (design 6.5).
    const int year   = field(suffix, 0, 4);
    const int month  = field(suffix, 5, 2);
    const int day    = field(suffix, 8, 2);
    const int hour   = field(suffix, 11, 2);
    const int minute = field(suffix, 14, 2);
    const int second = field(suffix, 17, 2);
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
        second > 59)
      return;

    // Optional `-N` same-second disambiguator, at least one digit and at most
    // kMaxCopyDigits of them: `copy` is an int and the suffix comes from a file
    // NAME, so an unbounded parse of `...-99999999999` is signed-overflow UB
    // and would order the entry by garbage.
    if (suffix.size() > kStampLength) {
      const std::size_t digits = suffix.size() - kStampLength - 1;
      if (suffix[kStampLength] != '-' || digits == 0 || digits > kMaxCopyDigits ||
          !all_digits(suffix, kStampLength + 1, digits))
        return;
      for (std::size_t i = kStampLength + 1; i < suffix.size(); ++i)
        *copy = *copy * 10 + (suffix[i] - '0');
    }

    *key = calendar_key(year, month, day, hour, minute, second);
  }

}  // namespace

std::int64_t stamp_key(const std::string &suffix) {
  std::int64_t key = 0;
  int copy         = 0;
  parse_stamp(suffix, &key, &copy);
  return key;
}

int stamp_copy_index(const std::string &suffix) {
  std::int64_t key = 0;
  int copy         = 0;
  parse_stamp(suffix, &key, &copy);
  return copy;
}

bool is_backup_stamp(const std::string &suffix) {
  return stamp_key(suffix) != 0;
}

std::filesystem::path backup_path_for(const std::filesystem::path &live,
                                      const std::string &stamp) {
  return live.parent_path() / (live.filename().string() + "." + stamp);
}

bool BackupResult::ok() const {
  return !files.empty() &&
         std::all_of(files.begin(), files.end(), [](const FileResult &f) {
           return f.ok;
         });
}

bool RestoreResult::ok() const {
  return !files.empty() &&
         std::all_of(files.begin(), files.end(), [](const FileResult &f) {
           return f.ok;
         });
}

bool RestoreResult::any_restored() const {
  return std::any_of(files.begin(), files.end(), [](const FileResult &f) {
    return f.ok;
  });
}

std::vector<std::filesystem::path>
backup_files(BackupKind kind, const profile::ProfileManager &profile) {
  switch (kind) {
  case BackupKind::LoadOrder:
    return {profile.plugins_path(), profile.loadorder_path(),
            profile.lockedorder_path()};
  case BackupKind::ModList:
    return {profile.modlist_path()};
  }
  return {};
}

std::vector<BackupEntry> list_backups(const std::filesystem::path &anchor) {
  auto entries = siblings_of(anchor);
  // Newest first by the parsed stamp, then by the same-second copy index
  // numerically; suffix-descending as the final tiebreak so the order is total
  // and deterministic. MO2 sorts by name and iterates in reverse
  // (mainwindow.cpp:3855-3867); the parsed key replaces the string comparison,
  // and the non-stamp branches (crash-orphan temporary, hand-made copy) sink
  // below every real backup, which is where a user should look last.
  std::stable_sort(entries.begin(), entries.end(),
                   [](const BackupEntry &a, const BackupEntry &b) {
                     if (a.stamp_key != b.stamp_key)
                       return a.stamp_key > b.stamp_key;
                     if (a.copy_index != b.copy_index)
                       return a.copy_index > b.copy_index;
                     return a.suffix > b.suffix;
                   });
  return entries;
}

BackupResult create_backup(const std::vector<std::filesystem::path> &files,
                           const FlushFn &flush,
                           std::chrono::system_clock::time_point now) {
  BackupResult result;
  if (files.empty())
    return result;

  // Flush FIRST (MO2 does too: savePluginList :3843, writeModlistNow :3920) so
  // the copy is of current state and not of whatever the debounced writer last
  // happened to emit. A flush that fails aborts the whole backup.
  if (!flush || !flush()) {
    for (const auto &file : files) {
      FileResult fr;
      fr.file    = file.filename().string();
      fr.failure = RestoreFailure::FlushFailed;
      result.files.push_back(std::move(fr));
    }
    return result;
  }

  return copy_with_stamp(files, unique_stamp(files, now));
}

BackupResult create_backup(BackupKind kind, const profile::ProfileManager &profile,
                           const FlushFn &flush,
                           std::chrono::system_clock::time_point now) {
  return create_backup(backup_files(kind, profile), flush, now);
}

RestoreResult restore_backup(const std::vector<std::filesystem::path> &files,
                             const std::string &stamp,
                             std::chrono::system_clock::time_point now) {
  RestoreResult result;
  if (files.empty())
    return result;

  // 1. Read every source up front. Reading later would race the safety backup's
  //    retention sweep, which can evict the very backup being restored - the
  //    oldest of ten is a perfectly ordinary thing to pick.
  std::vector<SourceRead> reads;
  reads.reserve(files.size());
  std::transform(files.begin(), files.end(), std::back_inserter(reads),
                 [&stamp](const std::filesystem::path &file) {
                   return read_source(backup_path_for(file, stamp));
                 });

  // 2. Safety backup of the live files, before the first write and attempted
  //    whatever happens next: MO2 has no such copy, so a wrong restore there
  //    leaves the user with nothing to go back to. No flush here - the point is
  //    to capture the on-disk bytes as they are right now. The stamp being
  //    restored is protected from the sweep so the copy cannot push the backup
  //    the user picked out of history.
  const auto safety   = copy_with_stamp(files, unique_stamp(files, now), {stamp});
  result.safety_stamp = safety.stamp;
  result.safety_ok    = safety.ok();

  // 3. Write each file independently. Never short-circuit: MO2 ||-chains its
  //    three copies (mainwindow.cpp:3904-3906), so a plugins.txt failure skips
  //    the other two and the profile ends up mixed with no statement that it
  //    is. Every failure is reported by name so the user knows which half of
  //    the profile is now stale.
  for (std::size_t i = 0; i < files.size(); ++i) {
    const auto &file = files[i];
    FileResult fr;
    fr.file = file.filename().string();

    if (!safety.files[i].ok) {
      // The safety copy of THIS file failed. Writing it now would destroy a
      // state there is no copy of, which is the one outcome the safety backup
      // exists to prevent - so leave it alone and say so. Its siblings still
      // restore, which is why this is per file and not a set-wide abort.
      fr.failure = RestoreFailure::SafetyBackupFailed;
      fr.detail  = safety.files[i].detail;
    } else if (!reads[i].exists) {
      fr.failure = RestoreFailure::NoSuchBackup;
      fr.detail  = stamp;
    } else if (!reads[i].readable) {
      fr.failure = RestoreFailure::UnreadableBackup;
    } else if (!profile::safe_write_file(file, reads[i].bytes)) {
      // safe_write_file reports a bool and nothing else; re-probe the target so
      // the report can name the reason instead of shrugging with "failed". The
      // probe opens WITHOUT creating, so a file that was missing cannot be left
      // behind as a 0-byte file that reads as a valid empty load order.
      fr.failure = file_is_openable(file) ? RestoreFailure::WriteFailed
                                          : RestoreFailure::NotWritable;
      fr.detail  = fr.failure == RestoreFailure::NotWritable
                       ? "permission denied"
                       : "the write did not complete";
    } else {
      fr.ok = true;
    }
    result.files.push_back(std::move(fr));
  }
  return result;
}

}  // namespace engine::backup