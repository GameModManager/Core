#pragma once

// Backward-compat wrapper — consumers should migrate to engine/source/router.h
#include "engine/source/router.h"

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

// Backward-compat aliases (deprecated — use Source::NxmLink / Source::Router)
using NxmLink  = Source::NxmLink;
using NxmRouter = Source::Router;

} // namespace engine
