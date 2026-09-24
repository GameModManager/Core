#include "engine/collection/choice_groups.h"

#include "engine/core/log/logger.h"

#include <algorithm>
#include <utility>

namespace engine::Collection {

namespace {

  bool same_members(const std::vector<std::string> &a,
                    const std::vector<std::string> &b) {
    if (a.size() != b.size()) {
      return false;
    }
    for (const auto &id : a) {
      if (std::find(b.begin(), b.end(), id) == b.end()) {
        return false;
      }
    }
    return true;
  }

}  // namespace

ChoiceValidation validate_choice_groups(const std::vector<ChoiceGroup> &groups,
                                        const ChoicePicks &picks) {
  ChoiceValidation result;

  for (const auto &[key, _] : picks) {
    const auto groupIt =
        std::find_if(groups.begin(), groups.end(), [&key](const auto &group) {
          return group.id == key;
        });
    if (groupIt == groups.end()) {
      result.unrecognized_groups.push_back(key);
      Logger::instance().debug("validate_choice_groups: no group with id '" + key +
                               "'");
    }
  }

  for (const auto &group : groups) {
    GroupVerdict verdict;
    verdict.group_id = group.id;

    const auto pickIt = picks.find(group.id);
    if (pickIt != picks.end()) {
      for (const auto &modId : pickIt->second) {
        const bool isMember =
            std::find(group.member_mod_ids.begin(), group.member_mod_ids.end(),
                      modId) != group.member_mod_ids.end();
        if (isMember) {
          ++verdict.valid_count;
          if (verdict.winning_pick.empty()) {
            verdict.winning_pick = modId;
          }
        } else {
          result.unrecognized_members.push_back(modId);
          Logger::instance().debug("validate_choice_groups: '" + modId +
                                   "' is not a member of group '" + group.id + "'");
        }
      }
    }

    if (verdict.valid_count > 1) {
      verdict.status = ChoiceStatus::TooMany;
      result.success = false;
    } else if (verdict.valid_count == 0 && group.mode == ChoiceMode::ExactlyOne) {
      verdict.status = ChoiceStatus::Empty;
      result.success = false;
    }
    result.verdicts.push_back(std::move(verdict));
  }

  return result;
}

ChoicePicks reconcile_prior_choices(const std::vector<ChoiceGroup> &groups,
                                    const PriorChoiceState &prior,
                                    const ChoicePicks &current_picks) {
  ChoicePicks effective = current_picks;

  for (const auto &group : groups) {
    const auto currentIt = effective.find(group.id);
    if (currentIt != effective.end() && !currentIt->second.empty()) {
      continue;  // explicit new pick wins over any remembered choice
    }

    const auto priorPickIt = prior.picks.find(group.id);
    if (priorPickIt == prior.picks.end() || priorPickIt->second.empty()) {
      effective.erase(group.id);
      continue;  // nothing remembered; leave the group unpicked
    }

    const auto priorMembersIt = prior.membership.find(group.id);
    if (priorMembersIt == prior.membership.end() ||
        !same_members(priorMembersIt->second, group.member_mod_ids)) {
      // Membership changed (or was never recorded): the remembered pick was
      // made against a different option set, so drop it and let the install
      // UI re-prompt instead of installing a stale choice.
      effective.erase(group.id);
      Logger::instance().debug(
          "reconcile_prior_choices: membership changed for group '" + group.id +
          "', dropping prior pick");
      continue;
    }

    // Membership identical: carry the remembered pick forward, keeping only
    // ids that are still members (defensive; the sets compared equal here).
    ChoicePicks::mapped_type carried;
    for (const auto &modId : priorPickIt->second) {
      if (std::find(group.member_mod_ids.begin(), group.member_mod_ids.end(), modId) !=
          group.member_mod_ids.end()) {
        carried.push_back(modId);
      }
    }
    if (carried.empty()) {
      effective.erase(group.id);
    } else {
      effective[group.id] = std::move(carried);
    }
  }

  // Drop picks for groups that no longer exist.
  for (auto it = effective.begin(); it != effective.end();) {
    const auto groupIt =
        std::find_if(groups.begin(), groups.end(), [&it](const auto &group) {
          return group.id == it->first;
        });
    if (groupIt == groups.end()) {
      Logger::instance().debug("reconcile_prior_choices: group '" + it->first +
                               "' no longer exists, dropping pick");
      it = effective.erase(it);
    } else {
      ++it;
    }
  }

  Logger::instance().info("reconcile_prior_choices: reconciled " +
                          std::to_string(effective.size()) + " group(s)");

  return effective;
}

}  // namespace engine::Collection
