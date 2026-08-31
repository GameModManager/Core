#pragma once

// Backward-compat wrapper - consumers should migrate to engine/source/nexus/auth.h
#include "engine/source/nexus/auth.h"

namespace engine {

// Backward-compat aliases (deprecated - use Source::Nexus::Auth / Source::Nexus::RateLimitInfo / Source::Nexus::NexusUserInfo)
using RateLimitInfo = Source::Nexus::RateLimitInfo;
using NexusUserInfo = Source::Nexus::NexusUserInfo;
using NexusAuth     = Source::Nexus::Auth;

} // namespace engine
