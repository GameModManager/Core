#include "engine/gmmpack/ini_edit_parser.h"

#include <algorithm>
#include <unordered_set>

namespace engine::gmmpack {

// ---------------------------------------------------------------------------
// Consolidation
// ---------------------------------------------------------------------------

IniEditConsolidation consolidate_ini_edits(const std::vector<IniEntry>& entries) {
    IniEditConsolidation result;

    for (const auto& entry : entries) {
        auto& file_con = result.by_target[entry.target_file];
        if (file_con.target_file.empty()) {
            file_con.target_file = entry.target_file;
        }

        for (const auto& edit : entry.edits) {
            file_con.edits.push_back(edit);

            // Index by source for retraction. Pack-author edits (null
            // sourceModId in JSON) have has_source_mod_id == false and an
            // empty source_mod_id; they group under "".
            const std::string key =
                edit.has_source_mod_id ? edit.source_mod_id : std::string{};
            result.by_source[key].push_back(edit);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Retraction - remove all edits attributed to a mod (update algorithm step 5)
// ---------------------------------------------------------------------------

std::vector<IniEdit> retract_mod_edits(IniEditConsolidation& consolidation,
                                        const std::string& mod_id) {
    std::vector<IniEdit> retracted;

    for (auto& [target, file_con] : consolidation.by_target) {
        auto new_end = std::remove_if(file_con.edits.begin(), file_con.edits.end(),
            [&](const IniEdit& e) {
                bool match = (!e.has_source_mod_id && mod_id.empty()) ||
                             (e.has_source_mod_id && e.source_mod_id == mod_id);
                if (match) retracted.push_back(e);
                return match;
            });
        file_con.edits.erase(new_end, file_con.edits.end());
    }

    // Rebuild by_source index (cheap - retraction is infrequent)
    consolidation.by_source.clear();
    for (auto& [target, file_con] : consolidation.by_target) {
        for (auto& edit : file_con.edits) {
            const std::string key =
                edit.has_source_mod_id ? edit.source_mod_id : std::string{};
            consolidation.by_source[key].push_back(edit);
        }
    }

    return retracted;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

Diagnostics validate_consolidated_edits(const IniEditConsolidation& consolidation) {
    Diagnostics diag;

    for (const auto& [target, file_con] : consolidation.by_target) {
        // Check for empty fields and duplicate section+key with different values
        // Key: section + "\0" + key -> first value seen
        std::unordered_map<std::string, std::pair<std::string, size_t>> seen;

        for (size_t i = 0; i < file_con.edits.size(); ++i) {
            const auto& edit = file_con.edits[i];
            std::string path = target + "/edits[" + std::to_string(i) + "]";

            if (edit.section.empty()) {
                diag.push_back({Diagnostic::Severity::Error, path,
                                "empty section"});
            }
            if (edit.key.empty()) {
                diag.push_back({Diagnostic::Severity::Error, path,
                                "empty key"});
            }
            if (edit.value.empty()) {
                diag.push_back({Diagnostic::Severity::Error, path,
                                "empty value"});
            }

            // Track duplicates
            std::string composite_key = edit.section + "\0" + edit.key;
            auto it = seen.find(composite_key);
            if (it != seen.end()) {
                if (it->second.first != edit.value) {
                    diag.push_back(
                        {Diagnostic::Severity::Warning, path,
                         "duplicate section/key with different value at index " +
                             std::to_string(it->second.second)});
                }
            } else {
                seen[composite_key] = {edit.value, i};
            }
        }
    }

    return diag;
}

// ---------------------------------------------------------------------------
// Collectors
// ---------------------------------------------------------------------------

std::vector<std::string> collect_sections(const IniEditConsolidation& consolidation) {
    std::unordered_set<std::string> sections;
    for (const auto& [target, file_con] : consolidation.by_target) {
        for (const auto& edit : file_con.edits) {
            sections.insert(edit.section);
        }
    }
    return {sections.begin(), sections.end()};
}

std::vector<std::string> collect_sources(const IniEditConsolidation& consolidation) {
    std::vector<std::string> sources;
    for (const auto& [src, edits] : consolidation.by_source) {
        sources.push_back(src);
    }
    return sources;
}

}  // namespace engine::gmmpack
