// settings_migration - the version gate and the zero-loss move rule.
//
// Retroactive renaming of the ini only works if an existing file's old keys
// become its new keys. Without this, every existing user silently resets to
// defaults on first run of a migrated build, which is the one failure mode this
// whole test exists to make impossible.
//
// This is REAL app code: settings_migration::migrate() is the exact function the
// private Settings ctor runs before the singleton is handed out. The test owns
// its own QSettings handle pointed at the sandboxed ini (same format, same
// org/app pair production uses) so it never depends on the Settings singleton's
// construction order, and it passes its OWN move table so the copy rule can be
// exercised while the production table is still empty.
//
// SAFETY. The shared Catch2 runner (tests/support/test_main.cpp) already flips
// the default format to IniFormat and relocates the UserScope path into a
// per-process temp dir, so every write lands there and the user's real
// ~/.config/GameModManager/GameModManager.conf is unreachable. There is
// deliberately NO mutation-proof that reverts a QSettings constructor here -
// that is how the real config got damaged. The negative control below breaks
// the GUARD, not the constructor, and the suite must be run under a throwaway
// XDG_CONFIG_HOME as well.

#include "ui/settings/settings_migration.h"

#include <QByteArray>
#include <QColor>
#include <QDateTime>
#include <QFile>
#include <QMap>
#include <QSettings>
#include <QStringList>
#include <QVariant>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace {

using settings_migration::Move;

// The sandboxed production store: the format the shared runner installed, and
// the org/app pair production constructs.
QSettings store() {
  return QSettings(QSettings::IniFormat, QSettings::UserScope, "GameModManager",
                   "GameModManager");
}

QSettings &fresh_store() {
  static QSettings s = store();
  s.clear();
  s.sync();
  return s;
}

// A whole-store snapshot. QMap so the comparison is over SORTED key/value pairs:
// QSettings keeps keys in a hash and allKeys() returns them in hash order, which
// is not the order they were written, and changes when a key is removed and
// re-added. Comparing the maps asserts what actually matters - no key added,
// removed or given a different value - without depending on that order.
QMap<QString, QVariant> snapshot(QSettings &s) {
  QMap<QString, QVariant> out;
  for (const QString &k : s.allKeys())
    out.insert(k, s.value(k));
  return out;
}

// A move at the running build's version, so it is inside (last, current] for
// whatever the gate decides "last" is - including the pre-migration case,
// where last is null and every version qualifies.
Move move(const char *old_key, const char *new_key) {
  return {QVersionNumber::fromString(QString(VERSION)), QString::fromLatin1(old_key),
          QString::fromLatin1(new_key)};
}

// Awkward values, one per thing IniFormat actually has to survive: the boolean
// and integer defaults are TRUE, so 0/false are the values that distinguish a
// carried value from a defaulted one; "" and the empty list are values QSettings
// is easy to confuse with "absent"; and the rest are the encodings - escapes,
// ini separators, a comment character, a newline, leading and trailing spaces -
// that a decode/re-encode round trip would mangle.
struct Awkward {
  const char *key;
  QVariant value;
};

std::vector<Awkward> awkward_values() {
  return {
      {"old/false", QVariant(false)},
      {"old/zero", QVariant(0)},
      {"old/empty_string", QVariant(QString())},
      {"old/empty_list", QVariant(QStringList())},
      // '=' is the key/value separator, ';' starts an ini comment, ',' is the
      // QStringList separator, and the backslashes are literal backslashes.
      {"old/separators", QVariant(QString("a=b;c,d\\e"))},
      {"old/newline", QVariant(QString("  lead and trail  \nsecond line  "))},
      {"old/colour", QVariant(QColor(1, 2, 3, 4))},
      {"old/stamp", QVariant(QDateTime(QDate(2026, 10, 11), QTime(12, 34, 56)))},
      {"old/blob", QVariant(QByteArray("\x00\x01\xff\x7f", 4))},
  };
}

}  // namespace

// ---------------------------------------------------------------------------
// The move rule: values cross verbatim, old keys go, a re-run is a no-op
// ---------------------------------------------------------------------------
TEST_CASE(
    "settings migration: awkward values move verbatim and a re-run changes nothing",
    "[ui][settings][migration]") {
  QSettings &s = fresh_store();

  for (const Awkward &a : awkward_values())
    s.setValue(QString::fromLatin1(a.key), a.value);
  // An unrelated key: the migration is a fixed table, never a whole-directory
  // move, so a key it does not name must survive untouched.
  s.setValue(QStringLiteral("untouched/someone_elses_key"), QStringLiteral("keep me"));
  s.sync();
  REQUIRE(!s.allKeys().isEmpty());

  const std::vector<Move> table = {
      move("old/false", "new/false"), move("old/zero", "new/zero"),
      move("old/empty_string", "new/empty_string"),
      move("old/empty_list", "new/empty_list"),
      move("old/separators", "new/separators"), move("old/newline", "new/newline"),
      move("old/colour", "new/colour"), move("old/stamp", "new/stamp"),
      move("old/blob", "new/blob"),
      // Never seeded, so its new key must not be
      // created: an absent old key writes nothing,
      // or "the user never chose this" stops being
      // distinguishable from "we chose the default".
      move("old/never_seeded", "new/never_seeded")};

  settings_migration::migrate(s, table);
  s.sync();

  CHECK_FALSE(s.contains(QStringLiteral("new/never_seeded")));

  for (const Awkward &a : awkward_values()) {
    const QString old_key = QString::fromLatin1(a.key);
    INFO("key " << a.key);
    // The exact QVariant, not a comparison that would also pass for a default.
    CHECK(s.value(QString(old_key).replace("old/", "new/")) == a.value);
    CHECK_FALSE(s.contains(old_key));
  }
  CHECK(s.value(QStringLiteral("untouched/someone_elses_key")).toString() ==
        QStringLiteral("keep me"));

  // Idempotence, two ways. (a) the gate: a second call finds the version
  // already current and must not touch the file at all. (b) the rule itself:
  // removing the version stamp puts the file back on the pre-migration branch,
  // so every move re-runs - and finds no old keys, so still writes nothing.
  // (b) is the one that would catch a move that re-creates its own key.
  const QMap<QString, QVariant> after_first = snapshot(s);
  QByteArray bytes_after_first;
  {
    QFile f(s.fileName());
    REQUIRE(f.open(QIODevice::ReadOnly));
    bytes_after_first = f.readAll();
  }

  settings_migration::migrate(s, table);
  s.sync();
  CHECK(snapshot(s) == after_first);

  s.remove(QStringLiteral("settings/version"));
  settings_migration::migrate(s, table);
  s.sync();
  CHECK(snapshot(s) == after_first);

  QFile f(s.fileName());
  REQUIRE(f.open(QIODevice::ReadOnly));
  // Every value is identical and the old keys are still gone after the forced
  // re-run, so the second pass did not write or re-encode anything.
  CHECK(f.readAll() == bytes_after_first);
}

// ---------------------------------------------------------------------------
// NEGATIVE CONTROL: the clobber guard. If this is green the control is broken.
// A user who already has a value under the NEW key keeps it; the old key goes.
// ---------------------------------------------------------------------------
TEST_CASE("settings migration: an existing new key wins and the old key is dropped",
          "[ui][settings][migration]") {
  QSettings &s = fresh_store();

  s.setValue(QStringLiteral("old/conflict"), QStringLiteral("old value"));
  s.setValue(QStringLiteral("old/identical"), QStringLiteral("same"));
  s.setValue(QStringLiteral("new/conflict"), QStringLiteral("new value"));
  s.setValue(QStringLiteral("new/identical"), QStringLiteral("same"));
  s.sync();

  const std::vector<Move> table = {move("old/conflict", "new/conflict"),
                                   move("old/identical", "new/identical")};
  settings_migration::migrate(s, table);
  s.sync();

  // The running build READS new/, so new/ is the value that is live. Letting
  // old/ win would silently revert a user who has already edited the new key.
  CHECK(s.value(QStringLiteral("new/conflict")).toString() ==
        QStringLiteral("new value"));
  CHECK_FALSE(s.contains(QStringLiteral("old/conflict")));

  // Identical on both sides is pure cleanup: old removed, value untouched.
  CHECK(s.value(QStringLiteral("new/identical")).toString() == QStringLiteral("same"));
  CHECK_FALSE(s.contains(QStringLiteral("old/identical")));
}

// ---------------------------------------------------------------------------
// The gate's four branches, and the "GMM has never written a version key" case
// MO2 does not need.
// ---------------------------------------------------------------------------
TEST_CASE("settings migration: the version gate", "[ui][settings][migration]") {
  const QVersionNumber current = QVersionNumber::fromString(QString(VERSION));
  const QString version_key(QStringLiteral("settings/version"));
  const std::vector<Move> table = {move("old/flag", "new/flag")};

  SECTION("fresh install: no version and no keys at all") {
    QSettings &s = fresh_store();
    REQUIRE(!s.contains(version_key));
    REQUIRE(s.allKeys().isEmpty());

    settings_migration::migrate(s, table);
    s.sync();

    // The move's old key is absent, so nothing may be written for it: absence
    // has to stay meaningful or the contains() readers downstream break.
    CHECK_FALSE(s.contains(QStringLiteral("new/flag")));
    CHECK(snapshot(s) == QMap<QString, QVariant>{{version_key, current.toString()}});
  }

  SECTION("pre-migration install: no version but the file has keys") {
    QSettings &s = fresh_store();
    s.setValue(QStringLiteral("old/flag"), QStringLiteral("carried"));
    s.sync();
    REQUIRE(!s.contains(version_key));

    // MO2 would classify this as fresh and migrate nothing, silently resetting
    // every existing user. This is the branch that prevents that.
    settings_migration::migrate(s, table);
    s.sync();

    CHECK(s.value(QStringLiteral("new/flag")).toString() == QStringLiteral("carried"));
    CHECK(s.value(version_key).toString() == current.toString());
  }

  SECTION("upgrade: a version below current runs the pending moves") {
    QSettings &s = fresh_store();
    s.setValue(QStringLiteral("old/flag"), QStringLiteral("carried"));
    s.setValue(version_key, QVersionNumber(0, 1, 0).toString());
    s.sync();

    settings_migration::migrate(s, table);
    s.sync();

    CHECK(s.value(QStringLiteral("new/flag")).toString() == QStringLiteral("carried"));
    CHECK(s.value(version_key).toString() == current.toString());
  }

  SECTION("downgrade: a version above current runs nothing and does not stamp") {
    QSettings &s        = fresh_store();
    const QString newer = QVersionNumber(current.segments().first(), 99, 0).toString();
    s.setValue(QStringLiteral("old/flag"), QStringLiteral("carried"));
    s.setValue(version_key, newer);
    s.sync();

    settings_migration::migrate(s, table);
    s.sync();

    CHECK_FALSE(s.contains(QStringLiteral("new/flag")));
    CHECK(s.value(version_key).toString() == newer);
  }

  SECTION("already current: nothing runs and nothing is rewritten") {
    QSettings &s = fresh_store();
    s.setValue(version_key, current.toString());
    s.sync();
    const QMap<QString, QVariant> before = snapshot(s);

    settings_migration::migrate(s, table);
    s.sync();

    CHECK(snapshot(s) == before);
  }
}

// ---------------------------------------------------------------------------
// The production table is still empty, so migrating a real store writes the
// version key and nothing else. This is what makes the stage independently
// shippable: it cannot lose a setting.
// ---------------------------------------------------------------------------
TEST_CASE("settings migration: the production table moves nothing",
          "[ui][settings][migration]") {
  QSettings &s = fresh_store();
  s.setValue(QStringLiteral("colors/modlist_contains_file"), QColor(1, 2, 3, 4));
  s.setValue(QStringLiteral("interface/lock_gui"), false);
  s.setValue(QStringLiteral("nexus/queue_downloads"), false);
  const QMap<QString, QVariant> before = snapshot(s);
  s.sync();
  REQUIRE(before.size() == 3);

  settings_migration::migrate(s);  // no table argument: production's
  s.sync();

  // Exactly one key added, nothing else touched.
  const QMap<QString, QVariant> after = snapshot(s);
  const QStringList added             = [&] {
    QStringList out;
    for (auto it = after.cbegin(); it != after.cend(); ++it)
      if (!before.contains(it.key()))
        out << it.key();
    return out;
  }();
  CHECK(added == QStringList{QStringLiteral("settings/version")});
  for (auto it = before.cbegin(); it != before.cend(); ++it)
    CHECK(after.value(it.key()) == it.value());

  CHECK(s.value(QStringLiteral("interface/lock_gui")).toBool() == false);
  CHECK(s.value(QStringLiteral("nexus/queue_downloads")).toBool() == false);
}