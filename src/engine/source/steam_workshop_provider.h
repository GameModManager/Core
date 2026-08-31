#pragma once

// Backward-compat wrapper - consumers should migrate to engine/source/steam/provider.h
#include "engine/source/steam/provider.h"

#include "engine/source/source_provider.h"

#include <memory>
#include <string>

namespace engine {

// Backward-compat alias (deprecated - use Source::Steam::Provider)
using SteamWorkshopProvider = Source::Steam::Provider;

} // namespace engine
