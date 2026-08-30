#pragma once

// Backward-compat wrapper — consumers should migrate to engine/source/nexus/account.h
#include "engine/source/nexus/account.h"

namespace engine {

// Backward-compat aliases (deprecated — use Source::Nexus::Account::* / Source::Nexus::NexusValidateResult)
using NexusValidateResult = Source::Nexus::NexusValidateResult;
using Source::Nexus::Account::validate_nexus_account;
using Source::Nexus::Account::parse_rate_limits;

} // namespace engine
