// Backward-compatible shim — include the canonical header instead.
#pragma once

#include "engine/deploy/vfs.h"

namespace engine {
using VfsStrategy = ::Deploy::Vfs;
}
