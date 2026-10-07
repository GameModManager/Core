#pragma once

// The gmmpack format's type surface, layered on the domain install model.
//
// The model itself (manifest, mods and their sources, executables, patches,
// tree, diagnostics) lives in engine/modpack/model.h: it is install INPUT, read
// by engine/install and engine/modpack/incremental_update, so it is not
// format-private. That header also re-exports the model into engine::gmmpack,
// so this one stays a single include for everything the format layer needs.
//
// The raw INI structs (IniTweak, IniEntry) stay in engine::gmmpack: they mirror
// ini/<targetFile>.json exactly and exist only as parser I/O. to_edit_file() in
// gmmpack/ini_edit_parser.h is the converter into the domain's typed tweaks.

#include "engine/modpack/model.h"