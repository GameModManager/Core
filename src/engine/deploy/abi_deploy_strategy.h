// Backward-compatible shim - include the canonical header instead.
#pragma once

#include "engine/deploy/abi_adapter.h"

namespace engine {
using AbiDeployStrategy = ::Deploy::AbiAdapter;
}
