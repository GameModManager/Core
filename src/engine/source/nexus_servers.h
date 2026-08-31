#pragma once

// Backward-compat wrapper - consumers should migrate to engine/source/nexus/servers.h
#include "engine/source/nexus/servers.h"

namespace engine {

// Backward-compat aliases (deprecated - use Source::Nexus::Servers / Source::Nexus::NexusServer)
using NexusServers = Source::Nexus::Servers;
using NexusServer  = Source::Nexus::NexusServer;

} // namespace engine
