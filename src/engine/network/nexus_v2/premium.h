#pragma once

// Premium detection for the Nexus download path. The account type is the one
// persisted by v1 users/validate.json (Source::Nexus::Account::validate_
// nexus_account) - no second source of truth, no extra request. The GraphQL
// API has no viewer/premium field for API-key auth; the v1 validate response
// (is_premium) is where Nexus reports it. Header-only, Qt-free.

#include "engine/source/nexus/auth.h"

namespace engine::nexus_v2
{

// True when the stored Nexus account is Premium (direct downloads, no
// nxm-key dance). False when unknown, Regular, or Supporter.
inline bool is_premium_user()
{
  const auto& auth = Source::Nexus::Auth::instance();
  return auth.has_user_info() && auth.get_user_info().account_type ==
                                     Source::Nexus::NexusUserInfo::AccountType::Premium;
}

}  // namespace engine::nexus_v2
