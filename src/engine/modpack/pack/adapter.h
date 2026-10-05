#pragma once

// Pack adapter value types - the one identity the installer carries for a mod
// it has resolved to something downloadable.
//
// A pack is either file-based (a .gmmpack archive on disk) or API-based (a
// Nexus collection reached through an nxm:// URL, resolved over the Nexus API
// - never as a raw file). Source classification lives in
// engine/modpack/pack/source_detector.h; this header carries the resolved result that
// conflict detection and append-install compare.
//
// Engine layer - Qt-free (only <string>).

#include <string>

namespace engine::Pack {

// A manifest entry resolved to something downloadable.
struct ResolvedMod {
  std::string entry_id;      // pack entry id this was resolved from
  std::string display_name;  // human-readable mod/file name for the UI
  std::string archive_name;  // real archive filename (empty = provider default)
  std::string source_type;   // provider to download through
  std::string source_id;
  std::string file_id;
};

}  // namespace engine::Pack
