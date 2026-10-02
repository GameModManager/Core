// Engine test for engine::backup - timestamped backup / restore points of a
// profile's load order and mod list (Workspace-czc0).
//
// MO2 parity, and the three deliberate divergences from it:
//   1. restore confirms before overwriting          (UI layer; MO2 is silent)
//   2. restore takes an UNCONDITIONAL safety backup (MO2 has none)
//   3. restore reports every file independently     (MO2 ||-chains its copies)
// plus eviction ordered by the PARSED stamp rather than by file-name sort.
//
// The engine layer is Qt-free and takes temp dirs only.
#include "engine/backup/backup_service.h"
#include "engine/profile/profile.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::atomic<int> g_counter{0};

fs::path make_temp_dir(const char *tag) {
  // NOT a "gmm*" prefix: install_method_test."the probe creates nothing" asserts
  // that nothing matching /tmp/gmm* appears while it runs, so a concurrently
  // running test that creates one makes it fail. Nothing here needs the prefix.
  auto dir = fs::temp_directory_path() /
             ("czc0_backup_" + std::string(tag) + "_" + std::to_string(getpid()) + "_" +
              std::to_string(g_counter.fetch_add(1)));
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

std::string read_text(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_text(const fs::path &p, const std::string &content) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << content;
}

// The suite runs with TZ=UTC (tests/CMakeLists.txt), so local time is UTC and
// a system_clock instant maps onto a stamp the test can spell out.
std::chrono::system_clock::time_point at(std::time_t epoch) {
  return std::chrono::system_clock::from_time_t(epoch);
}

// A fixed instant, so the stamp under test is a constant within one run.
constexpr std::time_t kStampInstant = 1790930755;

// The stamp the engine will write for `tp`. MO2's stamp is LOCAL time, so the
// expectation goes through localtime too: the suite runs with TZ=UTC, but the
// binary has to be correct when a developer runs it directly in another zone.
// The format itself is pinned separately, by stamp_key()'s own contract.
std::string stamp_of(std::chrono::system_clock::time_point tp) {
  const std::time_t seconds = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
  localtime_r(&seconds, &tm);
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%04d_%02d_%02d_%02d_%02d_%02d", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  return buf;
}

// A std::string, not a char*: every use here concatenates a file-name prefix
// onto it, and `const char* + const char*` has no overload visible here.
const std::string kStamp = stamp_of(at(kStampInstant));

std::vector<std::string> stamped_siblings(const fs::path &live) {
  const std::string prefix = live.filename().string() + ".";
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(live.parent_path(), ec)) {
    const auto name = entry.path().filename().string();
    if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0)
      names.push_back(name.substr(prefix.size()));
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::size_t count_stamped(const fs::path &live) {
  const auto suffixes = stamped_siblings(live);
  return static_cast<std::size_t>(
      std::count_if(suffixes.begin(), suffixes.end(), [](const std::string &suffix) {
        return engine::backup::is_backup_stamp(suffix);
      }));
}

}  // namespace

// The load-order backup is a BYTE-FOR-BYTE copy of the live file, taken AFTER
// the caller flushed the live state. A backup built from the pre-flush bytes -
// or from a re-serialisation - would be a backup of the wrong state, so the
// assertion pins the flushed content, not just the file's existence.
TEST_CASE("backup copies the live file byte-for-byte after the flush",
          "[engine][backup]") {
  const fs::path dir = make_temp_dir("copy");
  engine::profile::ProfileManager profile(dir, std::chrono::milliseconds{50});
  write_text(profile.loadorder_path(), "stale\n");
  write_text(profile.plugins_path(), "stale\n");
  write_text(profile.lockedorder_path(), "stale\n");

  // The flush stands in for PluginDb::Database::save_profile / write_modlist_now:
  // it is what makes the live file current, so the backup must see its output.
  bool flushed      = false;
  const auto result = engine::backup::create_backup(
      engine::backup::BackupKind::LoadOrder, profile,
      [&] {
        flushed = true;
        write_text(profile.loadorder_path(), "Skyrim.esm\nUpdate.esm\n");
        write_text(profile.plugins_path(), "*Skyrim.esm\n");
        return true;
      },
      at(kStampInstant));

  REQUIRE(flushed);
  REQUIRE(result.ok());
  CHECK(result.stamp == kStamp);
  REQUIRE(result.files.size() == 3);

  const fs::path backup = dir / (std::string("loadorder.txt.") + kStamp);
  REQUIRE(fs::is_regular_file(backup));
  CHECK(read_text(backup) == "Skyrim.esm\nUpdate.esm\n");
  // The other two files in the set share the one stamp.
  CHECK(fs::is_regular_file(dir / (std::string("plugins.txt.") + kStamp)));
  CHECK(fs::is_regular_file(dir / (std::string("lockedorder.txt.") + kStamp)));
  // The live files are untouched by the backup - only their siblings appear.
  CHECK(read_text(profile.loadorder_path()) == "Skyrim.esm\nUpdate.esm\n");

  fs::remove_all(dir);
}

// The flush is the step that makes the copy meaningful. When it fails the
// backup must fail with it: copying stale bytes and reporting success is the
// "backup that silently did nothing" defect (design 6.6).
TEST_CASE("backup reports a failed flush instead of copying stale bytes",
          "[engine][backup]") {
  const fs::path dir = make_temp_dir("flushfail");
  engine::profile::ProfileManager profile(dir, std::chrono::milliseconds{50});
  write_text(profile.modlist_path(), "+A\r\n");

  const auto result = engine::backup::create_backup(
      engine::backup::BackupKind::ModList, profile,
      [] {
        return false;
      },
      at(kStampInstant));

  CHECK_FALSE(result.ok());
  REQUIRE(result.files.size() == 1);
  CHECK(result.files.front().failure == engine::backup::RestoreFailure::FlushFailed);
  CHECK(stamped_siblings(profile.modlist_path()).empty());
  // The live file is still there, untouched.
  CHECK(read_text(profile.modlist_path()) == "+A\r\n");

  fs::remove_all(dir);
}

// A missing live file is reported per file; the sibling that does exist is
// still copied (never all-or-nothing).
TEST_CASE("backup reports a missing live file and keeps copying the rest",
          "[engine][backup]") {
  const fs::path dir = make_temp_dir("missing");
  engine::profile::ProfileManager profile(dir, std::chrono::milliseconds{50});
  write_text(profile.plugins_path(), "*Skyrim.esm\n");

  const auto result = engine::backup::create_backup(
      engine::backup::BackupKind::LoadOrder, profile,
      [] {
        return true;
      },
      at(kStampInstant));

  CHECK_FALSE(result.ok());
  REQUIRE(result.files.size() == 3);
  CHECK(result.files[0].ok);
  CHECK(result.files[1].failure == engine::backup::RestoreFailure::MissingLiveFile);
  CHECK(result.files[1].file == "loadorder.txt");
  CHECK(result.files[2].failure == engine::backup::RestoreFailure::MissingLiveFile);
  CHECK(fs::is_regular_file(dir / (std::string("plugins.txt.") + kStamp)));

  fs::remove_all(dir);
}

// Two backups inside one second must not overwrite each other. MO2 has no
// disambiguation and silently clobbers the first (design 6.7).
TEST_CASE("same-second backups are disambiguated, not overwritten",
          "[engine][backup]") {
  const fs::path dir = make_temp_dir("samesecond");
  engine::profile::ProfileManager profile(dir, std::chrono::milliseconds{50});
  write_text(profile.modlist_path(), "first\n");
  const auto flush = [] {
    return true;
  };

  const auto first = engine::backup::create_backup(engine::backup::BackupKind::ModList,
                                                   profile, flush, at(kStampInstant));
  write_text(profile.modlist_path(), "second\n");
  const auto second = engine::backup::create_backup(engine::backup::BackupKind::ModList,
                                                    profile, flush, at(kStampInstant));

  REQUIRE(first.ok());
  REQUIRE(second.ok());
  CHECK(first.stamp == kStamp);
  CHECK(second.stamp == std::string(kStamp) + "-2");
  CHECK(read_text(dir / (std::string("modlist.txt.") + kStamp)) == "first\n");
  CHECK(read_text(dir / (std::string("modlist.txt.") + kStamp + "-2")) == "second\n");

  fs::remove_all(dir);
}

// Retention is 10 per source file, swept independently per file, and only ever
// evicts a suffix that parses as a stamp. MO2's sweep uses a loose '?'-glob, so
// a hand-made copy or a shape-alike junk name is eligible for deletion whether
// or not it is a real backup (design 6.5).
TEST_CASE("retention keeps 10 stamped backups and never evicts anything else",
          "[engine][backup]") {
  const fs::path dir = make_temp_dir("keep");
  engine::profile::ProfileManager profile(dir, std::chrono::milliseconds{50});
  write_text(profile.modlist_path(), "live\n");

  // 12 stamps an hour apart the day before, oldest first. All 12 sort before the
  // stamp under test, so the three oldest seeded ones are the eviction victims.
  std::vector<std::string> seeded;
  for (int h = 0; h < 12; ++h) {
    // An hour apart, newest last; all 12 sort before the stamp under test.
    const std::string stamp = stamp_of(at(kStampInstant) - std::chrono::hours(12 - h));
    write_text(dir / ("modlist.txt." + stamp), "seed\n");
    seeded.push_back(stamp);
  }
  // Guard the fixture itself: if the seeded stamps did not sort before the
  // stamp under test the eviction assertions below would pass for free.
  REQUIRE(engine::backup::stamp_key(seeded.back()) < engine::backup::stamp_key(kStamp));

  // Siblings that are NOT backups. A retention sweep must leave every one of
  // them alone. None is six letters either, so none is mistaken for a
  // crash-orphan temporary either.
  write_text(dir / "modlist.txt.handmade-copy", "hand made\n");
  write_text(dir / "modlist.txt.ABCDEF", "orphan atomic temp\n");
  write_text(dir / "modlist.txt.9999_99_99_99_99_99", "junk\n");

  const auto result = engine::backup::create_backup(
      engine::backup::BackupKind::ModList, profile,
      [] {
        return true;
      },
      at(kStampInstant));
  REQUIRE(result.ok());

  CHECK(count_stamped(profile.modlist_path()) == 10);
  // The three oldest seeded stamps are gone, the nine newest seeded ones plus
  // the copy just made survive.
  CHECK_FALSE(fs::exists(dir / ("modlist.txt." + seeded[0])));
  CHECK_FALSE(fs::exists(dir / ("modlist.txt." + seeded[1])));
  CHECK_FALSE(fs::exists(dir / ("modlist.txt." + seeded[2])));
  for (std::size_t i = 3; i < seeded.size(); ++i)
    CHECK(fs::exists(dir / ("modlist.txt." + seeded[i])));
  CHECK(fs::exists(dir / (std::string("modlist.txt.") + kStamp)));
  // Non-stamps all survived.
  CHECK(fs::exists(dir / "modlist.txt.handmade-copy"));
  CHECK(fs::exists(dir / "modlist.txt.ABCDEF"));
  CHECK(fs::exists(dir / "modlist.txt.9999_99_99_99_99_99"));

  // The load-order set has its own independent history: nothing was created
  // for it, so it still has zero stamped backups.
  CHECK(count_stamped(profile.plugins_path()) == 0);

  fs::remove_all(dir);
}

// Ordering by the PARSED stamp, not by file-name sort. Eleven backups taken in
// the SAME second (the same-second disambiguator at its limit): a name sort
// puts "...-10" before "...-2" and would evict the newest of them, while the
// parsed copy index orders them numerically.
TEST_CASE("eviction orders the same-second suffix numerically, not by name",
          "[engine][backup]") {
  const fs::path dir = make_temp_dir("suffix");
  engine::profile::ProfileManager profile(dir, std::chrono::milliseconds{50});
  write_text(profile.modlist_path(), "live\n");

  // The plain stamp plus -2 .. -11: exactly what eleven same-second backups
  // produce, so the twelfth has to become -12.
  write_text(dir / ("modlist.txt." + kStamp), "seed\n");
  for (int i = 2; i <= 11; ++i)
    write_text(dir / (std::string("modlist.txt.") + kStamp + "-" + std::to_string(i)),
               "seed\n");
  REQUIRE(count_stamped(profile.modlist_path()) == 11);

  const auto result = engine::backup::create_backup(
      engine::backup::BackupKind::ModList, profile,
      [] {
        return true;
      },
      at(kStampInstant));
  REQUIRE(result.ok());
  // A twelfth copy of the same instant is disambiguated again.
  REQUIRE(result.stamp == std::string(kStamp) + "-12");

  CHECK(count_stamped(profile.modlist_path()) == 10);
  // The plain stamp is the oldest (copy index 0) and is evicted; "-10"
  // survives, which a name sort would have picked first.
  CHECK_FALSE(fs::exists(dir / (std::string("modlist.txt.") + kStamp)));
  CHECK(fs::exists(dir / (std::string("modlist.txt.") + kStamp + "-10")));
  CHECK(fs::exists(dir / (std::string("modlist.txt.") + kStamp + "-12")));

  fs::remove_all(dir);
}

// A backwards clock jump (or a DST fall-back making a local hour repeat) makes
// the copy just created the OLDEST by stamp. Evicting it on the spot would make
// the user's backup vanish - the newest thing they did, destroyed by the rule
// that is supposed to keep the newest ten.
TEST_CASE("a backwards clock jump never evicts the copy just created",
          "[engine][backup]") {
  const fs::path dir = make_temp_dir("backwards");
  engine::profile::ProfileManager profile(dir, std::chrono::milliseconds{50});
  write_text(profile.modlist_path(), "live\n");

  // Ten stamps a day AFTER the instant the backup will carry.
  std::vector<std::string> seeded;
  for (int d = 1; d <= 10; ++d) {
    // A day apart, all ten sort AFTER the instant the backup under test will
    // carry - which is the backwards clock jump being modelled.
    const std::string stamp = stamp_of(at(kStampInstant) + std::chrono::hours(24 * d));
    write_text(dir / ("modlist.txt." + stamp), "seed\n");
    seeded.push_back(stamp);
  }
  REQUIRE(count_stamped(profile.modlist_path()) == 10);

  const auto result = engine::backup::create_backup(
      engine::backup::BackupKind::ModList, profile,
      [] {
        return true;
      },
      at(kStampInstant));
  REQUIRE(result.ok());

  CHECK(count_stamped(profile.modlist_path()) == 10);
  // The new copy is the oldest by stamp but still there.
  CHECK(fs::is_regular_file(dir / (std::string("modlist.txt.") + kStamp)));
  // The oldest seeded stamp is the eviction victim instead.
  CHECK_FALSE(fs::exists(dir / ("modlist.txt." + seeded.front())));
  CHECK(fs::exists(dir / ("modlist.txt." + seeded.back())));

  fs::remove_all(dir);
}

// Stamp validation: the documented shape plus our same-second suffix. Anything
// else is a hand-made copy, never a backup the retention sweep may touch.
TEST_CASE("stamp validation accepts only a real stamp", "[engine][backup]") {
  using engine::backup::is_backup_stamp;

  CHECK(is_backup_stamp("2026_10_02_08_45_55"));
  CHECK(is_backup_stamp("2026_10_02_08_45_55-2"));
  CHECK(is_backup_stamp("2026_10_02_08_45_55-12"));

  // Right shape, impossible fields.
  CHECK_FALSE(is_backup_stamp("2026_13_02_08_45_55"));  // month 13
  CHECK_FALSE(is_backup_stamp("2026_10_32_08_45_55"));  // day 32
  CHECK_FALSE(is_backup_stamp("2026_10_02_24_45_55"));  // hour 24
  CHECK_FALSE(is_backup_stamp("2026_10_02_08_60_55"));  // minute 60
  CHECK_FALSE(is_backup_stamp("2026_10_02_08_45_60"));  // second 60

  // Wrong shape entirely.
  CHECK_FALSE(is_backup_stamp("9999_99_99_99_99_99"));
  CHECK_FALSE(is_backup_stamp("ABCDEF"));
  CHECK_FALSE(is_backup_stamp("handmade-copy"));
  CHECK_FALSE(is_backup_stamp("2026_10_02_08_45"));
  CHECK_FALSE(is_backup_stamp("2026_10_02_08_45_55x"));
  CHECK_FALSE(is_backup_stamp("2026_10_02_08_45_55-"));
  CHECK_FALSE(is_backup_stamp(""));

  // The parsed key is monotonic in the wall-clock fields, so a DST fall-back or
  // a clock change never reorders two real stamps.
  CHECK(engine::backup::stamp_key("2026_10_02_08_45_55") <
        engine::backup::stamp_key("2026_10_02_08_45_56"));
  CHECK(engine::backup::stamp_key("2026_10_02_08_45_55") <
        engine::backup::stamp_key("2026_11_01_02_30_00"));
  CHECK(engine::backup::stamp_key("2026_11_01_02_30_00") >
        engine::backup::stamp_key("2026_10_02_08_45_55"));
  CHECK(engine::backup::stamp_key("2026_10_02_08_45_55-2") ==
        engine::backup::stamp_key("2026_10_02_08_45_55"));
}

// The picker: newest first, and the three-way classification MO2 uses (real
// stamp / six-letter crash-orphan temporary / hand-made copy).
TEST_CASE("list_backups sorts newest first and classifies every suffix",
          "[engine][backup]") {
  const fs::path dir = make_temp_dir("list");
  engine::profile::ProfileManager profile(dir, std::chrono::milliseconds{50});
  const fs::path live = profile.loadorder_path();
  write_text(live, "live\n");

  write_text(dir / ("loadorder.txt.2026_09_01_10_00_00"), "old\n");
  write_text(dir / ("loadorder.txt.2026_10_02_08_45_55"), "middle\n");
  write_text(dir / ("loadorder.txt.2026_11_01_02_30_00-2"), "newest\n");
  write_text(dir / "loadorder.txt.ABCDEF", "orphan\n");
  write_text(dir / "loadorder.txt.handmade-copy", "hand\n");
  // A sibling of a DIFFERENT anchor must never be offered.
  write_text(dir / "plugins.txt.2026_12_01_00_00_00", "other anchor\n");

  const auto entries = engine::backup::list_backups(live);
  REQUIRE(entries.size() == 5);

  CHECK(entries[0].suffix == "2026_11_01_02_30_00-2");
  CHECK(entries[0].is_stamp);
  CHECK_FALSE(entries[0].is_orphan_temp);
  CHECK(entries[1].suffix == "2026_10_02_08_45_55");
  CHECK(entries[1].is_stamp);
  CHECK(entries[2].suffix == "2026_09_01_10_00_00");
  CHECK(entries[2].is_stamp);
  // Non-stamps sink below every real stamp, newest name first.
  CHECK(entries[3].suffix == "handmade-copy");
  CHECK_FALSE(entries[3].is_stamp);
  CHECK_FALSE(entries[3].is_orphan_temp);
  CHECK(entries[4].suffix == "ABCDEF");
  CHECK(entries[4].is_orphan_temp);

  CHECK(engine::backup::list_backups(dir / "no-such-file.txt").empty());

  fs::remove_all(dir);
}

// The happy-path restore: the chosen bytes land in the live file and the live
// file is unchanged in every other respect.
TEST_CASE("restore replaces the live files from the chosen stamp", "[engine][backup]") {
  const fs::path dir = make_temp_dir("restore");
  engine::profile::ProfileManager profile(dir, std::chrono::milliseconds{50});
  write_text(profile.plugins_path(), "current plugins\n");
  write_text(profile.loadorder_path(), "current order\n");
  write_text(profile.lockedorder_path(), "current locked\n");

  const auto files =
      engine::backup::backup_files(engine::backup::BackupKind::LoadOrder, profile);
  write_text(dir / (std::string("loadorder.txt.") + kStamp), "backed up order\n");
  write_text(dir / (std::string("plugins.txt.") + kStamp), "backed up plugins\n");

  // lockedorder.txt has no backup at this stamp - the "partial set" case.
  const auto result =
      engine::backup::restore_backup(files, kStamp, at(kStampInstant + 3600));

  CHECK(read_text(profile.loadorder_path()) == "backed up order\n");
  CHECK(read_text(profile.plugins_path()) == "backed up plugins\n");
  CHECK(read_text(profile.lockedorder_path()) == "current locked\n");

  REQUIRE(result.files.size() == 3);
  CHECK(result.files[0].ok);
  CHECK(result.files[1].ok);
  CHECK_FALSE(result.files[2].ok);
  CHECK(result.files[2].file == "lockedorder.txt");
  CHECK(result.files[2].failure == engine::backup::RestoreFailure::NoSuchBackup);

  fs::remove_all(dir);
}

// Divergence 3: every file is attempted and every failure is reported by name.
// MO2 ||-chains its three copies, so a plugins.txt failure skips the other two
// and leaves the profile silently mixed (mainwindow.cpp:3904-3906).
TEST_CASE("a failing file never stops the others and is named in the report",
          "[engine][backup]") {
  const fs::path dir = make_temp_dir("perfile");
  engine::profile::ProfileManager profile(dir, std::chrono::milliseconds{50});
  write_text(profile.plugins_path(), "current plugins\n");
  write_text(profile.loadorder_path(), "current order\n");
  write_text(profile.lockedorder_path(), "current locked\n");

  const auto files =
      engine::backup::backup_files(engine::backup::BackupKind::LoadOrder, profile);
  // The FIRST file in the set has no backup - exactly MO2's ||-chain trap.
  write_text(dir / (std::string("loadorder.txt.") + kStamp), "backed up order\n");
  write_text(dir / (std::string("lockedorder.txt.") + kStamp), "backed up locked\n");

  const auto result =
      engine::backup::restore_backup(files, kStamp, at(kStampInstant + 3600));

  CHECK_FALSE(result.ok());
  REQUIRE(result.files.size() == 3);

  REQUIRE(result.files[0].file == "plugins.txt");
  CHECK_FALSE(result.files[0].ok);
  CHECK(result.files[0].failure == engine::backup::RestoreFailure::NoSuchBackup);
  // Named, so the report can say WHICH file is now stale.
  CHECK_FALSE(result.files[0].file.empty());

  // The two later files still restored.
  CHECK(result.files[1].ok);
  CHECK(read_text(profile.loadorder_path()) == "backed up order\n");
  CHECK(result.files[2].ok);
  CHECK(read_text(profile.lockedorder_path()) == "backed up locked\n");

  fs::remove_all(dir);
}

// Divergence 3, second half: a backup that exists but cannot be read is
// reported as unreadable, not as "no such backup", and the live file is left
// alone. Mode 000 is the realistic shape - a backup on a profile directory the
// user has since locked down, or a file the OS refused to hand back.
//
// Root ignores the permission bits, so there is nothing to assert there; the
// case skips rather than passing for the wrong reason.
TEST_CASE("an unreadable backup is reported distinctly and writes nothing",
          "[engine][backup]") {
  if (geteuid() == 0) {
    WARN("running as root: mode 000 does not deny root, skipping");
    return;
  }
  const fs::path dir = make_temp_dir("unreadable");
  engine::profile::ProfileManager profile(dir, std::chrono::milliseconds{50});
  write_text(profile.modlist_path(), "live modlist\n");

  const fs::path source = dir / (std::string("modlist.txt.") + kStamp);
  write_text(source, "backed up\n");
  fs::permissions(source, fs::perms::none);

  const auto files =
      engine::backup::backup_files(engine::backup::BackupKind::ModList, profile);
  const auto result =
      engine::backup::restore_backup(files, kStamp, at(kStampInstant + 3600));

  CHECK_FALSE(result.ok());
  REQUIRE(result.files.size() == 1);
  CHECK_FALSE(result.files[0].ok);
  CHECK(result.files[0].failure == engine::backup::RestoreFailure::UnreadableBackup);
  CHECK(read_text(profile.modlist_path()) == "live modlist\n");

  fs::permissions(source, fs::perms::owner_all);
  fs::remove_all(dir);
}

// Divergence 2: the pre-restore safety backup is unconditional. MO2 has none,
// so a wrong restore there is unrecoverable. Here the pre-restore bytes are
// recoverable afterwards, which is the whole point of the feature.
TEST_CASE("restore takes an unconditional safety backup of the live files",
          "[engine][backup]") {
  const fs::path dir = make_temp_dir("safety");
  engine::profile::ProfileManager profile(dir, std::chrono::milliseconds{50});
  write_text(profile.modlist_path(), "state before the restore\n");
  write_text(dir / (std::string("modlist.txt.") + kStamp), "backed up\n");

  const auto files =
      engine::backup::backup_files(engine::backup::BackupKind::ModList, profile);
  const auto result =
      engine::backup::restore_backup(files, kStamp, at(kStampInstant + 3600));

  REQUIRE(result.ok());
  REQUIRE(result.safety_ok);
  CHECK_FALSE(result.safety_stamp.empty());
  CHECK(result.safety_stamp != kStamp);

  // The live file holds the restored bytes...
  CHECK(read_text(profile.modlist_path()) == "backed up\n");
  // ...and the pre-restore bytes are still on disk, under the safety stamp.
  const fs::path safety = dir / ("modlist.txt." + result.safety_stamp);
  REQUIRE(fs::is_regular_file(safety));
  CHECK(read_text(safety) == "state before the restore\n");

  fs::remove_all(dir);
}

// The safety backup is taken even when every restore file FAILS: the live state
// is about to be ambiguous, so the copy must not be conditional on success.
TEST_CASE("the safety backup is taken even when the restore fails",
          "[engine][backup]") {
  const fs::path dir = make_temp_dir("safetyfail");
  engine::profile::ProfileManager profile(dir, std::chrono::milliseconds{50});
  write_text(profile.modlist_path(), "state before the restore\n");

  const auto files =
      engine::backup::backup_files(engine::backup::BackupKind::ModList, profile);
  const auto result =
      engine::backup::restore_backup(files, kStamp, at(kStampInstant + 3600));

  CHECK_FALSE(result.ok());
  CHECK(result.safety_ok);
  CHECK(read_text(dir / ("modlist.txt." + result.safety_stamp)) ==
        "state before the restore\n");

  fs::remove_all(dir);
}

// The safety copy must never collide with the backup being restored, or the
// restore would read over its own source. Restoring the copy taken in the same
// second is the case that forces the check.
TEST_CASE("the safety stamp never collides with the stamp being restored",
          "[engine][backup]") {
  const fs::path dir = make_temp_dir("collide");
  engine::profile::ProfileManager profile(dir, std::chrono::milliseconds{50});
  write_text(profile.modlist_path(), "v1\n");
  const auto files =
      engine::backup::backup_files(engine::backup::BackupKind::ModList, profile);

  // A backup at the very instant the next restore runs from.
  const auto made = engine::backup::create_backup(
      engine::backup::BackupKind::ModList, profile,
      [] {
        return true;
      },
      at(kStampInstant));
  REQUIRE(made.ok());
  REQUIRE(made.stamp == kStamp);

  write_text(profile.modlist_path(), "v2\n");
  const auto result = engine::backup::restore_backup(files, kStamp, at(kStampInstant));

  CHECK(result.ok());
  CHECK(result.safety_stamp != kStamp);
  CHECK(fs::is_regular_file(dir / (std::string("modlist.txt.") + kStamp)));
  CHECK(read_text(profile.modlist_path()) == "v1\n");

  fs::remove_all(dir);
}

// backup_files() reads the paths off ProfileManager, so the four filenames
// live in exactly one place.
TEST_CASE("backup_files covers the four metadata files and nothing else",
          "[engine][backup]") {
  const fs::path dir = make_temp_dir("files");
  engine::profile::ProfileManager profile(dir, std::chrono::milliseconds{50});

  const auto load_order =
      engine::backup::backup_files(engine::backup::BackupKind::LoadOrder, profile);
  REQUIRE(load_order.size() == 3);
  CHECK(load_order[0] == profile.plugins_path());
  CHECK(load_order[1] == profile.loadorder_path());
  CHECK(load_order[2] == profile.lockedorder_path());

  const auto mod_list =
      engine::backup::backup_files(engine::backup::BackupKind::ModList, profile);
  REQUIRE(mod_list.size() == 1);
  CHECK(mod_list[0] == profile.modlist_path());

  // archives.txt and settings.ini are explicitly out of scope.
  for (const auto &f : load_order)
    CHECK(f.filename().string() != "archives.txt");
  for (const auto &f : mod_list)
    CHECK(f.filename().string() != "settings.ini");

  fs::remove_all(dir);
}