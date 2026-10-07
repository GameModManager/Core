#pragma once

// Collection manifest -> gmmpack translation (Nexus collection import path).
//
// Pure domain conversion between the two modpack descriptions: a
// Collection::Manifest (what a Nexus collection publishes) and a
// engine::gmmpack::Gmmpack (what the install wizard consumes). The mapping is
// field-for-field; see the .cpp for what a collection cannot carry.
//
// Engine layer - Qt-free.

#include "engine/modpack/collection/manifest.h"
#include "engine/modpack/model.h"

namespace engine::modpack {

// Build an in-memory gmmpack from a collection manifest: pack identity, info,
// tools, rules, load order, choice groups, and per-mod source entries.
// Nexus collections carry no patches, executables, INI tweaks, or tree data,
// so those stay empty and the wizard shows its empty-state notes for them.
[[nodiscard]] gmmpack::Gmmpack
manifest_to_gmmpack(const Collection::Manifest &manifest);

}  // namespace engine::modpack