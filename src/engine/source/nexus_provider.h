#pragma once

// Backward-compat wrapper - consumers should migrate to engine/source/nexus/provider.h
#include "engine/source/nexus/provider.h"

#include "engine/source/source_provider.h"

#include <string>

namespace engine {

// Backward-compat aliases (deprecated - use Source::Nexus::Provider / Source::Nexus::ModInfoResult)
using ModInfoResult  = Source::Nexus::ModInfoResult;
using NexusProvider  = Source::Nexus::Provider;

} // namespace engine
