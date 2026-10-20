// The page-update verdict ladder (engine/source/page_update.cpp).
//
// This is the whole decision the LoversLab and ModPub sweeps make per mod, and
// it is pure - no network, no disk - so every rung is tested against a table
// and nothing else. The ladder's order IS the feature: a page that states a
// version answers by version; a page that states only a date answers by date;
// a page that states neither can never be claimed as an update, because
// "probably newer" is the exact lie the Nexus sweep refuses to state for an
// unknown mod.
#include "engine/source/page_update.h"

#include <catch2/catch_test_macros.hpp>

using engine::Source::compare_page_update;
using engine::Source::PageUpdateOutcome;

TEST_CASE("page update ladder: version beats the known newest",
          "[engine][page_update]") {
  // The page states a version, and it is newer than the one the last check
  // recorded: an update, and the newest version is the page's.
  const auto v = compare_page_update("2.0", "2026-01-02T10:00:00+0000", "1.0",
                                     "2025-01-01T10:00:00+0000", "1.0");
  REQUIRE(v.outcome == PageUpdateOutcome::Update);
  REQUIRE(v.newest_version == "2.0");
}

TEST_CASE("page update ladder: same version as known newest is up to date",
          "[engine][page_update]") {
  const auto v = compare_page_update("1.0", "2025-01-01T10:00:00+0000", "1.0",
                                     "2025-01-01T10:00:00+0000", "1.0");
  REQUIRE(v.outcome == PageUpdateOutcome::Current);
  REQUIRE(v.newest_version == "1.0");
}

TEST_CASE("page update ladder: page version older than known newest is up to date",
          "[engine][page_update]") {
  // The known newest came from a previous check; a page that has REGRESSED
  // below it (author pulled a file) is not an update.
  const auto v = compare_page_update("0.9", "2024-01-01T10:00:00+0000", "1.0",
                                     "2025-01-01T10:00:00+0000", "0.9");
  REQUIRE(v.outcome == PageUpdateOutcome::Current);
}

TEST_CASE("page update ladder: no known newest falls to the date rung",
          "[engine][page_update]") {
  // First check ever: no known newest to compare against, so the page date
  // decides against the date the install recorded.
  const auto v = compare_page_update("2.0", "2026-01-02T10:00:00+0000", "",
                                     "2025-01-01T10:00:00+0000", "1.0");
  REQUIRE(v.outcome == PageUpdateOutcome::Update);
  REQUIRE(v.newest_version == "2.0");
}

TEST_CASE("page update ladder: date rung compares only the calendar day",
          "[engine][page_update]") {
  // The date prefix is 10 chars on purpose: the time-of-day and the timezone
  // suffix are noise a same-day re-upload would trip over. Same day, later
  // clock: NOT an update.
  const auto same_day = compare_page_update("", "2026-01-02T23:59:59+0000", "",
                                            "2026-01-02T10:00:00+0000", "1.0");
  REQUIRE(same_day.outcome == PageUpdateOutcome::Current);

  // A later DAY is an update even when the clock reads earlier.
  const auto later_day = compare_page_update("", "2026-01-03T01:00:00+0000", "",
                                             "2026-01-02T23:00:00+0000", "1.0");
  REQUIRE(later_day.outcome == PageUpdateOutcome::Update);
}

TEST_CASE("page update ladder: no recorded page state, the installed version decides",
          "[engine][page_update]") {
  // No page state recorded yet (first check, or the Mod Info panel's Refresh
  // basis): the page version against the installed version is the only basis,
  // and it is a real one - the page names something other than what the user
  // has.
  const auto v = compare_page_update("2.0", "2026-01-02T10:00:00+0000", "", "", "1.0");
  REQUIRE(v.outcome == PageUpdateOutcome::Update);
  REQUIRE(v.newest_version == "2.0");
}

TEST_CASE("page update ladder: a page that states nothing is NoBasis",
          "[engine][page_update]") {
  // ModPub's JSON-LD often carries no softwareVersion at all, and a page
  // without og:updated_time carries no date either. The honest verdict is
  // "cannot say", and the sweep records no verdict flag for it.
  const auto v = compare_page_update("", "", "", "", "1.0");
  REQUIRE(v.outcome == PageUpdateOutcome::NoBasis);
}

TEST_CASE("page update ladder: version rung needs an installed version to beat",
          "[engine][page_update]") {
  // The page states 2.0, nothing is known about what was seen before, and the
  // mod's own version is empty (a legacy install). There is no basis: "2.0 is
  // newer than nothing" is not a comparison.
  const auto v = compare_page_update("2.0", "2026-01-02T10:00:00+0000", "", "", "");
  REQUIRE(v.outcome == PageUpdateOutcome::NoBasis);
}

TEST_CASE("page update ladder: date rung still answers when the page has no version",
          "[engine][page_update]") {
  // The rung order matters here: softwareVersion is often empty from ModPub's
  // JSON-LD, and the date is the only signal the page gives. It must still be
  // able to answer.
  const auto v = compare_page_update("", "2026-01-02T10:00:00+0000", "1.0",
                                     "2025-01-01T10:00:00+0000", "1.0");
  REQUIRE(v.outcome == PageUpdateOutcome::Update);
  // The newest version is what the page could say: nothing. The sweep leaves
  // the recorded newestversion alone rather than overwrite it with empty.
  REQUIRE(v.newest_version.empty());
}
