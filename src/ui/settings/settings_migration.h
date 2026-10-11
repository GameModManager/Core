#pragma once

#include <QSettings>
#include <QString>
#include <QVersionNumber>

#include <span>
#include <vector>

// One-shot settings migration. Runs once per process, from the private Settings
// constructor, so nothing in the process can observe an un-migrated ini - the
// first read is Core::Application's max_core_dumps() at app/core.cpp.
//
// The gate is MO2's (references/modorganizer/src/settings.cpp:104,
// Settings::processUpdates): the ini carries the version that last wrote it and
// every migration whose version falls in (last, current] runs. migrate() adds
// the one branch MO2 does not need - see version_gate() in the .cpp.
namespace settings_migration {

// One key move. The QVariant is carried across verbatim and never decoded, so
// a single entry covers bool, int, QString, QStringList, QByteArray, QColor and
// QDateTime alike.
struct Move {
  QVersionNumber version;
  QString old_key;
  QString new_key;
};

// The production table. EMPTY in this stage: no key has moved yet, so
// migrate() stamps settings/version and writes nothing else. Each later stage
// that moves keys appends one row.
inline const std::vector<Move> kMoves{};

// Runs the pending migrations in `moves`, then stamps settings/version with the
// running build's version. Defaults to the production table.
void migrate(QSettings &s, std::span<const Move> moves = kMoves);

}  // namespace settings_migration