// Backward-compatible shim - include the canonical header instead.
#pragma once

#include "engine/deploy/direct.h"

namespace engine {
using DirectDeployStrategy = ::Deploy::Direct;
using SyncResult = ::Deploy::SyncResult;
using DeployedFileInfo = ::Deploy::DeployedFileInfo;
} // namespace engine
