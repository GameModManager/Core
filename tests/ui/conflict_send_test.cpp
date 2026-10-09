// Offscreen GUI test for the mod list's Send to First/Last Conflict actions.
//
// MO2 ModListViewActions::sendModsToFirstConflict / sendModsToLastConflict
// (modlistviewactions.cpp:731-776) take every mod the selection overwrites
// (getModOverwrite) or is overwritten by (getModOverwritten), map them to
// priorities, and move the selection to the LOWEST of the first set and the
// HIGHEST of the second. In this model the row index is the priority and
// ConflictPairs already carries both directions, so the mapper picks the
// smallest row among wins_against and the largest among loses_to.
//
// What this pins, beyond the arithmetic:
//   - the mapper skips a partner that has no row (row_of returns -1) instead
//     of treating row -1 as the minimum, which would silently send the mod to
//     the top of the list,
//   - the action's result REACHES THE MODEL: move_mod runs and the row order
//     changes. A getter round-trip would pass even with a dead consumer.
//   - no partners in a direction means -1, so the menu gate hides the entry
//     rather than offering an action that cannot move anything.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, no network.
#include "ui/widgets/mod_list_model.h"

#include <QApplication>
#include <QMap>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <string>
#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

// The exact lambda the menu gate and ModActions use, wrapped so a test
// failure names the mapping rather than a raw std::function.
int target_for(const ui::ModList &model, const QString &mod_id, bool first) {
  const auto &pairs = model.conflict_pairs();
  auto it           = pairs.constFind(mod_id);
  if (it == pairs.constEnd())
    return -1;
  const ui::ModList *m = &model;
  return ui::conflict_send_target(
      *it,
      [m](const QString &other) {
        return m->priority_of(other);
      },
      first);
}
}  // namespace

TEST_CASE("send to first/last conflict", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_conflict_send/config";
  std::filesystem::remove_all("/tmp/gmm_conflict_send");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);

  // Rows 0..3 are user mods; MO2 order, so a LARGER row is a HIGHER
  // priority number = the mod that wins.
  ui::ModList model;
  QVector<ui::ModEntry> entries;
  for (const char *id : {"Alpha", "Beta", "Gamma", "Delta"}) {
    ui::ModEntry e;
    e.id      = QString::fromLatin1(id);
    e.name    = e.id;
    e.enabled = true;
    entries.append(e);
  }
  model.reset_with_order(entries);

  QMap<QString, ui::ConflictPairs> pairs;
  // Alpha (row 0) wins against Gamma (row 2) and loses to Delta (row 3).
  pairs[QStringLiteral("Alpha")].wins_against = {QStringLiteral("Gamma")};
  pairs[QStringLiteral("Alpha")].loses_to     = {QStringLiteral("Delta")};
  model.set_conflict_pairs(pairs);

  SECTION("first conflict takes the LOWEST row among the mods it wins against") {
    check(target_for(model, QStringLiteral("Alpha"), true) == 2,
          "Alpha wins only against Gamma, which sits at row 2");
  }

  // A single partner makes min and max coincide, so the two directions would
  // be indistinguishable. These two sections are the ones that can fail when
  // the mapper's min/max is swapped.
  SECTION("with several partners, First is the topmost and Last the bottom-most") {
    // Alpha beats Gamma (2) and Delta (3); Beta (1) and Delta (3) beat Alpha.
    pairs[QStringLiteral("Alpha")].wins_against = {QStringLiteral("Gamma"),
                                                   QStringLiteral("Delta")};
    pairs[QStringLiteral("Alpha")].loses_to     = {QStringLiteral("Beta"),
                                                   QStringLiteral("Delta")};
    model.set_conflict_pairs(pairs);
    check(target_for(model, QStringLiteral("Alpha"), true) == 2,
          "First Conflict stops at Gamma, the topmost mod Alpha overwrites");
    check(target_for(model, QStringLiteral("Alpha"), false) == 3,
          "Last Conflict stops at Delta, the bottom-most mod that overwrites Alpha");
    check(target_for(model, QStringLiteral("Alpha"), true) !=
              target_for(model, QStringLiteral("Alpha"), false),
          "the two directions must not collapse onto the same partner");
  }

  SECTION("last conflict takes the HIGHEST row among the mods that win against it") {
    check(target_for(model, QStringLiteral("Alpha"), false) == 3,
          "Alpha loses only to Delta, which sits at row 3");
  }

  SECTION("a partner with no row is skipped, not treated as row -1") {
    // "Ghost" is in the conflict map but is not a row the model lists. If the
    // mapper took row -1 at face value it would return -1 and the mod would
    // move to the top of the list (row 0) instead of to Gamma's row.
    pairs[QStringLiteral("Alpha")].wins_against = {QStringLiteral("Ghost"),
                                                   QStringLiteral("Gamma")};
    model.set_conflict_pairs(pairs);
    check(target_for(model, QStringLiteral("Alpha"), true) == 2,
          "an unlisted partner is skipped, Gamma at row 2 still wins the min");
  }

  SECTION("no partners in a direction means no target") {
    check(target_for(model, QStringLiteral("Alpha"), false) == 3,
          "sanity: the loses_to direction is populated");
    pairs[QStringLiteral("Alpha")].wins_against.clear();
    model.set_conflict_pairs(pairs);
    check(target_for(model, QStringLiteral("Alpha"), true) == -1,
          "an empty wins_against set has no First Conflict target");
    check(target_for(model, QStringLiteral("Nobody"), true) == -1,
          "a mod absent from the conflict map has no target");
  }

  SECTION("the target REACHES THE MODEL: the row order actually changes") {
    const int target = target_for(model, QStringLiteral("Alpha"), false);
    REQUIRE(target >= 0);
    model.move_mod(QStringLiteral("Alpha"), target);
    // Last Conflict pushes the mod as far DOWN as it can go while still
    // losing to the bottom-most mod that beats it. Alpha loses to Delta
    // (row 3) and wins against Gamma (row 2), so Alpha ends up directly
    // below Delta - list order is Beta, Gamma, Delta, Alpha. A getter
    // round-trip would pass here even with a dead consumer; only the
    // renumbered priorities show the move reached the model.
    check(model.priority_of(QStringLiteral("Beta")) == 0, "Beta holds the top row");
    check(model.priority_of(QStringLiteral("Gamma")) == 1, "Gamma sits at row 1");
    check(model.priority_of(QStringLiteral("Delta")) == 2,
          "Delta shifted up one when Alpha was lifted out ahead of it");
    check(model.priority_of(QStringLiteral("Alpha")) == 3,
          "Alpha landed directly below Delta, still losing to it");
  }

  SECTION("First Conflict lifts the mod to just below its topmost partner") {
    // Alpha wins against Gamma (row 2); the topmost mod it overwrites is
    // Gamma, so Alpha stops there instead of staying pinned at the top.
    const int target = target_for(model, QStringLiteral("Alpha"), true);
    REQUIRE(target >= 0);
    model.move_mod(QStringLiteral("Alpha"), target);
    check(model.priority_of(QStringLiteral("Alpha")) == 2,
          "Alpha sits immediately below Gamma and still overwrites it");
    check(model.priority_of(QStringLiteral("Delta")) == 3,
          "Delta still overwrites Alpha: the win/loss pairs are unchanged");
  }
}