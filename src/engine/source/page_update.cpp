#include "engine/source/page_update.h"

#include <algorithm>

namespace engine::Source {

namespace {

  // Both sides of the date comparison are cut to the YYYY-MM-DD prefix: one
  // side may carry a time of day ("2025-06-05T14:23:11") and the other may not
  // ("2025-06-05"), and comparing those as strings would call a same-day
  // republish an update. Equal-length ISO dates order lexicographically.
  std::string date_prefix(const std::string &iso) {
    return iso.size() >= 10 ? iso.substr(0, 10) : iso;
  }

}  // namespace

PageUpdateVerdict compare_page_update(const std::string &page_version,
                                      const std::string &page_date,
                                      const std::string &known_newest,
                                      const std::string &known_date,
                                      const std::string &installed_version) {
  PageUpdateVerdict verdict;
  verdict.newest_version = page_version;

  // Whether ANY comparison was possible - the difference between Current
  // ("compared, nothing moved") and NoBasis ("could not compare at all").
  bool comparable = false;

  // 1. The page names a version and the mod records one, and they differ:
  //    the page is offering something other than what is installed. This is
  //    the user's ground truth, so it answers before the recorded page state
  //    does - a page that REGRESSED to the installed version is not an
  //    update, whatever the last check recorded as newest.
  if (!page_version.empty() && !installed_version.empty()) {
    comparable = true;
    if (page_version != installed_version) {
      verdict.outcome = PageUpdateOutcome::Update;
      return verdict;
    }
    // Equal: the version question is settled, but a same-version
    // REPUBLISH is still possible - the date rung below catches it.
  }
  // 2. The page's day is later than the day the last check recorded: the
  //    page moved since we looked, whatever the version string says.
  if (!page_date.empty() && !known_date.empty()) {
    comparable = true;
    if (date_prefix(page_date) > date_prefix(known_date)) {
      verdict.outcome = PageUpdateOutcome::Update;
      return verdict;
    }
  }
  // 3. No installed version to compare against (a legacy install): the page
  //    version against the version the last check recorded is the only
  //    version basis left. Only reached when rung 1 could not run - with an
  //    installed version recorded, rung 1 already settled the version
  //    question and this rung would turn a regressed page into a badge.
  if (installed_version.empty() && !page_version.empty() && !known_newest.empty()) {
    comparable = true;
    if (page_version != known_newest) {
      verdict.outcome = PageUpdateOutcome::Update;
      return verdict;
    }
  }
  // 4. Compared and nothing moved, or nothing to compare against. The
  //    caller records the page state without a verdict in the NoBasis case,
  //    so the next check has a basis.
  verdict.outcome =
      comparable ? PageUpdateOutcome::Current : PageUpdateOutcome::NoBasis;
  return verdict;
}

}  // namespace engine::Source
