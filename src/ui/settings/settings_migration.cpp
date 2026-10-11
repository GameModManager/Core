#include "ui/settings/settings_migration.h"

#include <QLoggingCategory>
#include <QStringList>

namespace settings_migration {
namespace {

  // MO2's General/version, flattened into our root settings/ group.
  constexpr const char *kVersionKey = "settings/version";

  // THE ZERO-LOSS MOVE RULE. Three properties carry the whole argument:
  //
  //  1. An absent old key writes NOTHING. The key stays absent, so the
  //     contains()-based logic downstream keeps working -
  //     nexus_queue_downloads_set(), ensure_modlist_column_defaults() and the
  //     retired check_update_after_install fallback all read "absent" as "the
  //     user never chose". Writing a default here would collapse "the user
  //     chose the default" into "we chose it".
  //  2. A present NEW key is the guard: the stored value is never overwritten,
  //     and the old key is simply dropped. Same precedence the retired-key
  //     fallback already uses (settings.cpp:185-191), generalised.
  //  3. The QVariant is copied VERBATIM - never decoded to a
  //     bool/int/QColor/... and re-encoded. QSettings decodes its own escapes
  //     (@DateTime(...), @ByteArray, @String(...), #aarrggbb) into the right
  //     type on read and re-encodes them identically on write, so a verbatim
  //     copy is lossless for every type we store. A typed intermediate is the
  //     only place loss could enter.
  //
  // Note on downgrade-then-upgrade (3c in the design note): because the old key
  // is DELETED rather than left behind, a downgrade re-creates it empty, and
  // the following upgrade sees the new key present - so it keeps the newer
  // value and discards the stale one. That is the correct outcome and it is the
  // reason the old key goes. Do not "fix" this by keeping the old key.
  void move_key(QSettings &s, const Move &m) {
    if (!s.contains(m.old_key))
      return;
    if (s.contains(m.new_key)) {
      s.remove(m.old_key);
      qInfo("settings migration: kept the new key, dropped %s in favour of %s",
            qUtf8Printable(m.old_key), qUtf8Printable(m.new_key));
      return;
    }
    s.setValue(m.new_key, s.value(m.old_key));
    s.remove(m.old_key);
  }

  void run_pending(QSettings &s, std::span<const Move> moves,
                   const QVersionNumber &last, const QVersionNumber &current) {
    for (const Move &m : moves) {
      if (last < m.version && m.version <= current)
        move_key(s, m);
    }
  }

  void stamp(QSettings &s, const QVersionNumber &current) {
    s.setValue(kVersionKey, current.toString());
  }

  // THE BRANCH MO2 DOES NOT NEED. MO2 has written General/version since 2.2.0,
  // so "version absent" reliably means "fresh install" there and it stamps and
  // returns. GMM has NEVER written a version key, so for the entire existing
  // userbase "absent" means "pre-migration install" - and naive MO2 semantics
  // would classify every existing user as fresh, run nothing, and silently
  // reset 100% of their settings on the first run of a migrated build. The
  // file's own contents are what tell the two apart: allKeys() is read-only and
  // creates no keys, so a genuinely fresh profile stays genuinely fresh.
  void version_gate(QSettings &s, std::span<const Move> moves) {
    const QVersionNumber current = QVersionNumber::fromString(QString(VERSION));

    if (!s.contains(kVersionKey)) {
      if (s.allKeys().isEmpty()) {
        stamp(s, current);  // fresh install: stamp only, migrate nothing
        return;
      }
      // Pre-migration install: run EVERY migration, since "last version" is
      // unknown and may predate all of them.
      run_pending(s, moves, QVersionNumber(), current);
      stamp(s, current);
      return;
    }

    const QVersionNumber last =
        QVersionNumber::fromString(s.value(kVersionKey).toString());
    if (last > current)
      return;  // downgrade: run nothing, and do not stamp - a newer version's
               // stamp must survive so the upgrade back runs correctly
    if (last == current)
      return;  // nothing pending, and re-stamping would rewrite the file
    run_pending(s, moves, last, current);
    stamp(s, current);
  }

}  // namespace

void migrate(QSettings &s, std::span<const Move> moves) {
  version_gate(s, moves);
}

}  // namespace settings_migration
