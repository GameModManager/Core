// The mod list's AND/OR filter join. Non-UI on purpose: the rule is what
// decides whether a row survives a filter, and getting it wrong silently
// empties or un-filters the whole list - a failure the widget cannot show
// and the click-driven suite would never notice.
#include "engine/filter/filter_combine.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

namespace {

using engine::filter::Criterion;
using engine::filter::Mode;

bool run(Mode mode, const Criterion *criteria, std::size_t count) {
  return engine::filter::matches(mode, criteria, count);
}

}  // namespace

TEST_CASE("no active filter matches every mod", "[filter]") {
  const std::array<Criterion, 0> none{};
  CHECK(run(Mode::And, none.data(), none.size()));
  CHECK(run(Mode::Or, none.data(), none.size()));

  // All four mod-list criteria present but every one inactive: the empty
  // filter box, the "All" group, an untouched category panel and no special
  // filter ticked. Switching every filter off must never empty the list.
  const std::array inactive{Criterion{false, false}, Criterion{false, true},
                            Criterion{false, false}, Criterion{false, false}};
  CHECK(run(Mode::And, inactive.data(), inactive.size()));
  CHECK(run(Mode::Or, inactive.data(), inactive.size()));
}

TEST_CASE("an inactive criterion never votes", "[filter]") {
  // And: the one active criterion matches, the others are inactive, so the
  // mod survives even though the inactive ones carry matched=false.
  const std::array and_mode{Criterion{true, true}, Criterion{false, false},
                            Criterion{false, false}};
  CHECK(run(Mode::And, and_mode.data(), and_mode.size()));

  // Or: one active criterion that did NOT match hides the mod. The inactive
  // criteria that "matched" must not rescue it.
  const std::array or_mode{Criterion{true, false}, Criterion{false, true},
                           Criterion{false, true}};
  CHECK_FALSE(run(Mode::Or, or_mode.data(), or_mode.size()));
}

TEST_CASE("And needs every active criterion to match", "[filter]") {
  const std::array all_true{Criterion{true, true}, Criterion{true, true},
                            Criterion{true, true}};
  CHECK(run(Mode::And, all_true.data(), all_true.size()));

  const std::array one_false{Criterion{true, true}, Criterion{true, false},
                             Criterion{true, true}};
  CHECK_FALSE(run(Mode::And, one_false.data(), one_false.size()));
}

TEST_CASE("the same criteria can go either way on the mode", "[filter]") {
  // The one input pair that separates And from Or: two active criteria, one
  // matching and one refusing. A join that ignored the mode would have to
  // return the same verdict for both, so exactly one of these can pass.
  const std::array split{Criterion{true, true}, Criterion{true, false}};
  CHECK(run(Mode::And, split.data(), split.size()) ==
        !run(Mode::Or, split.data(), split.size()));
  CHECK_FALSE(run(Mode::And, split.data(), split.size()));
  CHECK(run(Mode::Or, split.data(), split.size()));
}

TEST_CASE("Or needs only one active criterion to match", "[filter]") {
  const std::array one_true{Criterion{true, false}, Criterion{true, true},
                            Criterion{true, false}};
  CHECK(run(Mode::Or, one_true.data(), one_true.size()));

  // Every active criterion refusing the mod hides it: "no match" is a real
  // verdict under Or, not a pass-through.
  const std::array all_false{Criterion{true, false}, Criterion{true, false},
                             Criterion{true, false}};
  CHECK_FALSE(run(Mode::Or, all_false.data(), all_false.size()));
}