// Backward-compatible shim — include the canonical header instead.
#pragma once

#include "engine/deploy/overlay_fs_kernel.h"

namespace engine {
using OverlayFsStrategy = ::Deploy::OverlayFsKernel;
}
