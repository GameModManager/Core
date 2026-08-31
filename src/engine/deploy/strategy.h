// Backward-compatible shim — include the canonical headers instead.
#pragma once

#include "engine/deploy/interface.h"
#include "engine/deploy/overlay_fs_deploy.h"
#include "engine/deploy/symlink.h"

namespace engine {
using DeploymentStrategy = ::Deploy::Interface;
using SymlinkStrategy = ::Deploy::Symlink;
using OverlayFsDeployStrategy = ::Deploy::OverlayFsDeploy;
} // namespace engine
