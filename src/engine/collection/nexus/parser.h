#pragma once

// Nexus Mods collection.json parser.
//
// Parses the Vortex-compatible collection.json format (as exported by Nexus
// Mods) and maps it to the source-agnostic Collection::Manifest interface.
// This is the adapter from Nexus format to the internal format.
//
// Engine layer - Qt-free. Depends on nlohmann/json for parsing.

#include "engine/collection/manifest.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace engine::Collection::Nexus {

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

// Parse a Nexus collection.json string into a Collection::Manifest.
//
// The JSON format is the one produced by Vortex's collection export:
//   { "info": {...}, "mods": [...], "modRules": [...] }
//
// Throws ParseError on malformed or semantically invalid JSON.
Manifest parse(std::string_view json);

// Convenience overload: parse from a file path.
Manifest parse_file(const std::string& path);

} // namespace engine::Collection::Nexus
