// Backward-compatible shim — include the canonical header instead.
#pragma once

#include "engine/deploy/ledger.h"

namespace engine {
using DeployEntry = ::Deploy::Entry;
using DeployLedger = ::Deploy::Ledger;
} // namespace engine
