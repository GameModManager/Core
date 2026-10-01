#pragma once

// Internal helpers shared across self-updater implementations. Not part of the
// public API - include only from engine/update/*.cpp files.

#include "engine/update/self_updater.h"

#include "engine/core/github.h"

namespace engine::update {

// Query the GitHub releases API for the latest release and parse the tag
// against the compiled-in VERSION. asset_suffix is matched against asset
// filenames (e.g. ".exe", ".dmg", ".AppImage"); an empty suffix never matches,
// because a zero-length suffix compares equal to every name and would
// otherwise hand back whichever asset GitHub happens to list first.
// include_prereleases selects the endpoint - see GitHub::releases_endpoint -
// and is honoured, not ignored. Returns UpdateInfo with available=true only
// when the remote tag is strictly newer.
UpdateInfo fetch_update_info(const std::string &asset_suffix,
                             bool include_prereleases = false);

// Find the download URL of the first asset whose name ends with suffix.
// Returns empty for an empty suffix or when no asset matches.
std::string find_asset_url(const engine::GitHub::Release &release,
                           const std::string &suffix);

}  // namespace engine::update