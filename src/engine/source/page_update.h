#pragma once

// The update verdict for a page-scrape source (LoversLab, ModPub): what one
// fresh page scrape says against what the mod's meta.ini last recorded.
//
// These sources have no API and no file ids to compare, so "is there a newer
// version" is answered by scraping the mod's public page and comparing it
// against the page state a previous check (or the Mod Info panel's Refresh)
// recorded. The comparison is pure and lives here so both providers decide
// identically and the ladder is testable with no network.
//
// The rule is honest by construction: a scrape that cannot be compared
// against anything recorded is NoBasis - the caller records the page state
// WITHOUT a verdict, so the next check has a basis, and this one shows no
// badge. A fabricated "up to date" is the one answer this must never produce.

#include <string>

namespace engine::Source {

enum class PageUpdateOutcome {
  Update,   // the page moved since the state we hold was recorded
  Current,  // the page is exactly the state we hold
  NoBasis,  // nothing on either side to compare - record, no verdict
};

struct PageUpdateVerdict {
  PageUpdateOutcome outcome = PageUpdateOutcome::NoBasis;
  std::string newest_version;  // the page's version string, when it stated one
};

// Compare one fresh page scrape against the state a mod's meta.ini holds.
//
//   page_version        the page's softwareVersion ("" when it stated none)
//   page_date           the page's dateModified ("" when it stated none)
//   known_newest        [General] newestversion as recorded ("" when never)
//   known_date          [<source>] date_modified as recorded ("" when never)
//   installed_version   [General] version as recorded ("" when never)
//
// Ladder, first match wins:
//   1. page version vs the INSTALLED version - the user's ground truth: the
//      page is offering something other than what they have. A page that
//      regressed to the installed version is NOT an update, whatever the
//      last check recorded.
//   2. page date vs the date a previous check recorded - catches a
//      same-version republication. Both sides are cut to the YYYY-MM-DD
//      prefix first, because one side may carry a time of day and the other
//      may not; lexicographic order of equal-length ISO dates is
//      chronological order.
//   3. page version vs the version a previous check recorded - the basis
//      when no installed version exists (a legacy install). Never reached
//      when rung 1 ran: with an installed version recorded, that rung
//      already settled the version question.
//   4. nothing to compare: NoBasis.
PageUpdateVerdict compare_page_update(const std::string &page_version,
                                      const std::string &page_date,
                                      const std::string &known_newest,
                                      const std::string &known_date,
                                      const std::string &installed_version);

}  // namespace engine::Source
