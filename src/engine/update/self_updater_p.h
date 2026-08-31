#pragma once

// Internal helpers shared across self-updater implementations. Not part of the
// public API - include only from engine/update/*.cpp files.

#include "engine/update/self_updater.h"

#include <nlohmann/json.hpp>

namespace engine::update {

// Query the GitHub releases API for the latest release and parse the tag
// against the compiled-in VERSION. asset_suffix is matched against asset
// filenames (e.g. ".exe", ".dmg", ".AppImage"). include_prereleases gates
// pre-release flags. Returns UpdateInfo with available=true only when the
// remote tag is strictly newer.
UpdateInfo fetch_update_info(const std::string &asset_suffix,
                             bool include_prereleases = false);

} // namespace engine::update
