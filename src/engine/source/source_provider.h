#pragma once

// Backward-compat wrapper - consumers should migrate to engine/source/interface.h
// and engine/source/registry.h directly.
#include "engine/source/interface.h"
#include "engine/source/registry.h"

namespace engine {

// Backward-compat aliases (deprecated - use Source::Interface / Source::Registry)
using SourceDownloadInfo = Source::SourceDownloadInfo;
using SourceProvider     = Source::Interface;
using SourceRegistry     = Source::Registry;

} // namespace engine
