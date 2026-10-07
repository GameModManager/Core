#pragma once

#include <cstddef>

namespace engine::filter {

// How the mod list joins the filters that are switched on at the same time.
// And keeps a mod only when every active filter matches it; Or keeps it when
// any one of them does. MO2's ModListSortProxy::FilterAnd / FilterOr
// (references/modorganizer/src/modlistsortproxy.h:35-38).
enum class Mode { And, Or };

// One filter's verdict for one mod. An inactive filter is not a filter: it
// never votes and never narrows the list, so an empty search box and an
// untouched category panel stay out of the combination entirely instead of
// counting as a filter that matched nothing.
struct Criterion {
  bool active  = false;
  bool matched = false;
};

// Join the verdicts. With nothing switched on there is nothing to filter by,
// so the mod matches: turning every filter off must never empty the list.
// Header-only because the rule is a handful of counters and the mod list
// calls it once per row on every keystroke - no allocation, no indirection.
[[nodiscard]] inline bool matches(Mode mode, const Criterion *criteria,
                                  std::size_t count) {
  bool any_active = false;
  bool all_match  = true;
  bool any_match  = false;
  for (std::size_t i = 0; i < count; ++i) {
    if (!criteria[i].active)
      continue;
    any_active = true;
    if (criteria[i].matched)
      any_match = true;
    else
      all_match = false;
  }
  if (!any_active)
    return true;
  return mode == Mode::And ? all_match : any_match;
}

}  // namespace engine::filter