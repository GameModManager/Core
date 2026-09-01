#pragma once

// Backward-compat wrapper - consumers should migrate to
// engine/source/loverslab/provider.h
#include "engine/source/loverslab/provider.h"

#include "engine/source/source_provider.h"

#include <string>

namespace engine {

// Backward-compat aliases (deprecated - use Source::LoversLab::Provider /
// Source::LoversLab::ModInfoResult).
using LoversLabProvider = Source::LoversLab::Provider;
using LoversLabModInfoResult = Source::LoversLab::ModInfoResult;

} // namespace engine
