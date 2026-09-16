#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/gmmpack/types.h"
#include "engine/modpack/ini_edits.h"

namespace engine::gmmpack {

// ---------------------------------------------------------------------------
// INI tweak parsing - the thin gmmpack layer for ini/*.json
//
// This layer reads JSON and produces IniTweak structs. Everything downstream
// (content parsing, merge, apply, retract) lives in engine/modpack/ini_edits
// and is shared with the non-gmmpack paths: there is exactly one INI engine.
// ---------------------------------------------------------------------------

// Parse one ini/<targetFile>.json document (already schema-validated by the
// unpack pipeline) into an IniEntry. Lenient by design: missing fields fall
// back to defaults, since strict shape checking belongs to the schema stage.
IniEntry parse_ini_entry(const nlohmann::json& j);

// Convert a parsed entry to the engine's tweak structs (notably mapping the
// raw status string onto modpack::TweakStatus). Unknown status strings map
// to Recommended; schema-validated input never hits that path.
modpack::IniEditFile to_edit_file(const IniEntry& entry);

}  // namespace engine::gmmpack
