#pragma once

// Backward-compat wrapper — consumers should migrate to engine/source/loverslab/auth.h
#include "engine/source/loverslab/auth.h"

#include "engine/core/keyring/keyring.h"

#include <filesystem>
#include <memory>
#include <string>

namespace engine {

// Backward-compat alias (deprecated — use Source::LoversLab::Auth)
using LoversLabAuth = Source::LoversLab::Auth;

} // namespace engine
