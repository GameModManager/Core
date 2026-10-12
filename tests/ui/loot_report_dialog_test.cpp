// The report body of the LOOT popup (ui/loot_report_text), the one piece of
// new logic in the port of MO2's LootDialog report pane.
//
// MO2 builds this text in Loot::Report::successMarkdown
// (references/modorganizer/src/loot.cpp:240-274), with the per-message prefix
// from Loot::Message::toMarkdown (loot.cpp:391-412) and the trailing stats
// line from Loot::Stats::toMarkdown (loot.cpp:303-308). The branch being pinned
// is the level prefix: an error and a warning must not render the same, and an
// info message must stay bare, because that is the only place the user ever
// sees which level LOOT gave a message.
#include "ui/widgets/loot_report_dialog.h"

#include <catch2/catch_test_macros.hpp>
#include <string>

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

engine::Sorter::Loot::Result::Message msg(std::string level, std::string text) {
  return engine::Sorter::Loot::Result::Message{std::move(level), std::move(text)};
}
}  // namespace

TEST_CASE("loot report text marks the message level", "[ui][loot]") {
  using engine::Sorter::Loot::Stats;

  Stats stats;
  stats.time            = 1234;
  stats.lootcli_version = "0.19.0";
  stats.loot_version    = "0.19.2";

  // The happy path: all three levels in one run, the order LOOT gave them.
  const QString text = ui::loot_report_text(
      {msg("error", "Skyrim.esm is missing"), msg("warning", "Shorthand is obsolete"),
       msg("info", "The masterlist is up to date")},
      stats);

  check(text.contains("### General messages"),
        "the game-wide messages carry MO2's own heading");
  check(text.contains("- **Error**: Skyrim.esm is missing"),
        "an error message is prefixed with Error");
  check(text.contains("- **Warning**: Shorthand is obsolete"),
        "a warning message is prefixed with Warning");
  check(text.contains("- The masterlist is up to date"),
        "an info message carries no prefix, as in MO2");
  check(text.contains("`stats: 1.23s, lootcli 0.19.0, loot 0.19.2`"),
        "the trailing stats line is MO2's, with the time in seconds");

  // Every message keeps its own line, so one line is never flattened into
  // another (this is the property the old console relay had and lost nothing
  // of, kept because the popup is now the only surface).
  check(text.count(QLatin1String("\n - ")) == 3,
        "one report line per message, none merged");

  // A run with nothing to say says so, in MO2's wording, instead of leaving an
  // empty pane that looks like a popup that failed to populate.
  const QString empty = ui::loot_report_text({}, Stats{});
  check(empty.contains("**No messages.**"), "a report with no messages still says so");
  check(!empty.contains("### General messages"),
        "no heading is printed when there is nothing under it");

  // "warn" is normalised to "warning" by loot_message_level
  // (engine/sort/sorter/loot/sorter.cpp:103-109) before it reaches here, so a
  // raw "warn" is the only level that has no prefix - which is the same
  // rendering an info message gets. Pinned so the two are not confused later.
  const QString raw_warn = ui::loot_report_text({msg("warn", "raw")}, Stats{});
  check(!raw_warn.contains("**Warning**"),
        "the raw JSON type is normalised upstream, never rendered raw");
}