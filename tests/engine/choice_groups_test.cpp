// Generic choice-group validation (exactly-one / at-most-one) and
// prior-choice reconciliation for incremental updates.
// Source-agnostic - operates on Collection::ChoiceGroup picks only.

#include "engine/collection/choice_groups.h"

#include <string>
#include <catch2/catch_test_macros.hpp>

namespace {

using namespace engine::Collection;

ChoiceGroup make_group(const std::string& id, ChoiceMode mode)
{
  ChoiceGroup group;
  group.id = id;
  group.name = id;
  group.mode = mode;
  group.member_mod_ids = {"tex-a", "tex-b", "tex-c"};
  return group;
}

const GroupVerdict& verdict_for(
    const ChoiceValidation& validation, const std::string& id)
{
  for (const auto& verdict : validation.verdicts) {
    if (verdict.group_id == id) {
      return verdict;
    }
  }
  FAIL("no verdict for group '" << id << "'");
  static const GroupVerdict fallback;
  return fallback;
}

PriorChoiceState make_prior(const ChoicePicks& picks, const ChoicePicks& membership)
{
  PriorChoiceState prior;
  prior.picks = picks;
  prior.membership = membership;
  return prior;
}

std::vector<ChoiceGroup> single_group(const std::string& id, ChoiceMode mode)
{
  return {make_group(id, mode)};
}

} // namespace

// ---------------------------------------------------------------------------
// validate_choice_groups: exactly-one
// ---------------------------------------------------------------------------

TEST_CASE("Exactly-one group with a single pick is valid", "[collection][choice]")
{
  const auto groups = single_group("textures", ChoiceMode::ExactlyOne);

  ChoicePicks picks;
  picks["textures"] = {"tex-b"};

  const auto validation = validate_choice_groups(groups, picks);

  REQUIRE(validation.success);
  REQUIRE(validation.verdicts.size() == 1);
  const auto& verdict = verdict_for(validation, "textures");
  REQUIRE(verdict.status == ChoiceStatus::Valid);
  REQUIRE(verdict.valid_count == 1);
  REQUIRE(verdict.winning_pick == "tex-b");
}

TEST_CASE("Exactly-one group with no pick reports Empty", "[collection][choice]")
{
  const auto groups = single_group("textures", ChoiceMode::ExactlyOne);

  const auto validation = validate_choice_groups(groups, {});

  REQUIRE_FALSE(validation.success);
  const auto& verdict = verdict_for(validation, "textures");
  REQUIRE(verdict.status == ChoiceStatus::Empty);
  REQUIRE(verdict.valid_count == 0);
  REQUIRE(verdict.winning_pick.empty());
}

TEST_CASE("Exactly-one group with two picks reports TooMany", "[collection][choice]")
{
  const auto groups = single_group("textures", ChoiceMode::ExactlyOne);

  ChoicePicks picks;
  picks["textures"] = {"tex-a", "tex-c"};

  const auto validation = validate_choice_groups(groups, picks);

  REQUIRE_FALSE(validation.success);
  const auto& verdict = verdict_for(validation, "textures");
  REQUIRE(verdict.status == ChoiceStatus::TooMany);
  REQUIRE(verdict.valid_count == 2);
  REQUIRE(verdict.winning_pick == "tex-a");
}

// ---------------------------------------------------------------------------
// validate_choice_groups: at-most-one
// ---------------------------------------------------------------------------

TEST_CASE("At-most-one group allows zero or one pick", "[collection][choice]")
{
  const auto groups = single_group("optional-hd", ChoiceMode::AtMostOne);

  const auto empty = validate_choice_groups(groups, {});
  REQUIRE(empty.success);
  REQUIRE(verdict_for(empty, "optional-hd").status == ChoiceStatus::Valid);

  ChoicePicks one;
  one["optional-hd"] = {"tex-a"};
  const auto single = validate_choice_groups(groups, one);
  REQUIRE(single.success);
  REQUIRE(verdict_for(single, "optional-hd").winning_pick == "tex-a");
}

TEST_CASE("At-most-one group with two picks reports TooMany", "[collection][choice]")
{
  const auto groups = single_group("optional-hd", ChoiceMode::AtMostOne);

  ChoicePicks picks;
  picks["optional-hd"] = {"tex-a", "tex-b"};

  const auto validation = validate_choice_groups(groups, picks);

  REQUIRE_FALSE(validation.success);
  REQUIRE(verdict_for(validation, "optional-hd").status == ChoiceStatus::TooMany);
}

// ---------------------------------------------------------------------------
// validate_choice_groups: diagnostics
// ---------------------------------------------------------------------------

TEST_CASE("Unknown groups and non-member picks are reported, not counted", "[collection][choice]")
{
  const auto groups = single_group("textures", ChoiceMode::ExactlyOne);

  ChoicePicks picks;
  picks["textures"] = {"tex-a", "not-a-mod"};
  picks["no-such-group"] = {"tex-a"};

  const auto validation = validate_choice_groups(groups, picks);

  REQUIRE(validation.success); // diagnostics alone do not fail validation
  REQUIRE(validation.unrecognized_groups == std::vector<std::string>{"no-such-group"});
  REQUIRE(validation.unrecognized_members == std::vector<std::string>{"not-a-mod"});
  const auto& verdict = verdict_for(validation, "textures");
  REQUIRE(verdict.valid_count == 1);
  REQUIRE(verdict.winning_pick == "tex-a");
}

TEST_CASE("Verdicts follow group order", "[collection][choice]")
{
  const std::vector<ChoiceGroup> groups = {make_group("first", ChoiceMode::ExactlyOne),
      make_group("second", ChoiceMode::AtMostOne)};

  const auto validation = validate_choice_groups(groups, {});

  REQUIRE(validation.verdicts.size() == 2);
  REQUIRE(validation.verdicts[0].group_id == "first");
  REQUIRE(validation.verdicts[1].group_id == "second");
  REQUIRE_FALSE(validation.success); // "first" is exactly-one and empty
}

// ---------------------------------------------------------------------------
// reconcile_prior_choices
// ---------------------------------------------------------------------------

TEST_CASE("Prior pick is remembered when membership is identical", "[collection][choice]")
{
  const auto groups = single_group("textures", ChoiceMode::ExactlyOne);

  ChoicePicks prior_picks;
  prior_picks["textures"] = {"tex-b"};
  ChoicePicks membership;
  membership["textures"] = {"tex-a", "tex-b", "tex-c"};

  const auto effective =
      reconcile_prior_choices(groups, make_prior(prior_picks, membership), {});

  REQUIRE(effective.size() == 1);
  REQUIRE(effective.at("textures") == std::vector<std::string>{"tex-b"});
}

TEST_CASE("Prior pick is dropped when membership changed", "[collection][choice]")
{
  ChoiceGroup group = make_group("textures", ChoiceMode::ExactlyOne);
  group.member_mod_ids = {"tex-a", "tex-b", "tex-c", "tex-d"}; // new option added
  const std::vector<ChoiceGroup> groups = {group};

  ChoicePicks prior_picks;
  prior_picks["textures"] = {"tex-b"};
  ChoicePicks membership;
  membership["textures"] = {"tex-a", "tex-b", "tex-c"}; // snapshot at pick time

  const auto effective =
      reconcile_prior_choices(groups, make_prior(prior_picks, membership), {});

  REQUIRE(effective.find("textures") == effective.end());
}

TEST_CASE("Prior pick is dropped when the group no longer exists", "[collection][choice]")
{
  const std::vector<ChoiceGroup> groups; // pack revision removed all groups

  ChoicePicks prior_picks;
  prior_picks["textures"] = {"tex-b"};
  ChoicePicks membership;
  membership["textures"] = {"tex-a", "tex-b", "tex-c"};

  const auto effective =
      reconcile_prior_choices(groups, make_prior(prior_picks, membership), {});

  REQUIRE(effective.empty());
}

TEST_CASE("Explicit current pick wins over the remembered pick", "[collection][choice]")
{
  const auto groups = single_group("textures", ChoiceMode::ExactlyOne);

  ChoicePicks prior_picks;
  prior_picks["textures"] = {"tex-a"};
  ChoicePicks membership;
  membership["textures"] = {"tex-a", "tex-b", "tex-c"};
  ChoicePicks current;
  current["textures"] = {"tex-c"};

  const auto effective = reconcile_prior_choices(
      groups, make_prior(prior_picks, membership), current);

  REQUIRE(effective.at("textures") == std::vector<std::string>{"tex-c"});
}

TEST_CASE("Prior pick without a membership snapshot is dropped", "[collection][choice]")
{
  const auto groups = single_group("textures", ChoiceMode::ExactlyOne);

  ChoicePicks prior_picks;
  prior_picks["textures"] = {"tex-a"};

  // No membership snapshot: cannot prove the options are unchanged.
  const auto effective =
      reconcile_prior_choices(groups, make_prior(prior_picks, {}), {});

  REQUIRE(effective.find("textures") == effective.end());
}
