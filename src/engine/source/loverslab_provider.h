#pragma once

// Backward-compat wrapper - consumers should migrate to engine/source/loverslab/provider.h
#include "engine/source/loverslab/provider.h"

#include "engine/source/source_provider.h"

#include <string>

namespace engine {

// Backward-compat alias (deprecated - use Source::LoversLab::Provider)
using LoversLabProvider = Source::LoversLab::Provider;

} // namespace engine
