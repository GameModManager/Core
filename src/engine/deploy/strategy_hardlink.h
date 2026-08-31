// Backward-compatible shim - include the canonical header instead.
#pragma once

#include "engine/deploy/hardlink.h"

namespace engine {
using HardlinkStrategy = ::Deploy::Hardlink;
}
