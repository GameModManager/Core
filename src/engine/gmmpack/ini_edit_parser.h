#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "engine/gmmpack/types.h"

namespace engine::gmmpack {

// ---------------------------------------------------------------------------
// Consolidated INI edits - the engine layer for ini/*.json
//
// Each .gmmpack archive contains ini/<targetFile>.json files where every edit
// to a given INI file (from any mod or the pack author) is consolidated into
// one place. This module parses those files, groups edits by target file,
// tracks source attribution, and supports the update algorithm's retraction
// step (removing edits whose owning mod was removed).
// ---------------------------------------------------------------------------

// Consolidated edits for a single target INI file.
struct IniFileConsolidation {
    std::string target_file;
    std::vector<IniEdit> edits;  // all edits, in order from the JSON
};

// Full consolidated result across all target files in a pack.
struct IniEditConsolidation {
    // target_file (as-is from JSON) -> consolidation
    std::unordered_map<std::string, IniFileConsolidation> by_target;

    // source key ("" = pack-author, else mod id) -> copies of edits
    // across all files. Copies, not pointers: by_target vectors reallocate
    // on push_back, so pointers into them would dangle.
    std::unordered_map<std::string, std::vector<IniEdit>> by_source;
};

// ---------------------------------------------------------------------------
// Core operations
// ---------------------------------------------------------------------------

// Build a consolidated view from parsed IniEntry objects (one per ini/*.json).
// Groups edits by target file and by source mod.
IniEditConsolidation consolidate_ini_edits(const std::vector<IniEntry>& entries);

// Retract all edits attributed to a given mod from the consolidation.
// Returns edits that were retracted (for the install widget's diff display).
// The consolidation is mutated in place: retracted edits are removed.
std::vector<IniEdit> retract_mod_edits(IniEditConsolidation& consolidation,
                                        const std::string& mod_id);

// Validate consolidated edits: detect duplicate section+key with different
// values (ambiguity), empty fields, and section/key ordering issues.
Diagnostics validate_consolidated_edits(const IniEditConsolidation& consolidation);

// Collect all unique section names across all target files.
std::vector<std::string> collect_sections(const IniEditConsolidation& consolidation);

// Collect all unique source mod IDs (empty string included for pack-author).
std::vector<std::string> collect_sources(const IniEditConsolidation& consolidation);

}  // namespace engine::gmmpack
