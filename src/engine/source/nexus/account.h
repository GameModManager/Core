#pragma once

#include <string>

namespace engine::Source::Nexus {

// Result of validating the stored Nexus API key against users/validate.json.
struct ValidateResult {
  bool ok = false;     // true = key accepted and user info persisted
  std::string message; // human-readable failure reason (empty on success)
};

// Backward-compat alias (deprecated - use ValidateResult directly).
using NexusValidateResult = ValidateResult;

// Account namespace: validates the stored API key and parses rate-limit
// headers.
namespace Account {

// Validates the stored API key (users/validate.json): updates the persisted
// rate-limit state from the response headers and stores the account info
// (user id / name / account type) via NexusAuth. Synchronous, Qt-free.
// Returns ok=false without touching stored state when no key is configured.
ValidateResult validate_nexus_account();

// Parses the Nexus rate-limit headers (x-rl[-authenticated]-{daily,hourly}-
// {limit,remaining,reset}) out of a raw header block and persists them via
// NexusAuth::update_rate_limit. Shared by all Nexus API responses.
void parse_rate_limits(const std::string &headers);

} // namespace Account
} // namespace engine::Source::Nexus
