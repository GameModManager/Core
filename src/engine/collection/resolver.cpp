#include "engine/collection/resolver.h"

#include <algorithm>
#include <unordered_set>

namespace engine::Collection {

// ---------------------------------------------------------------------------
// ModSource visitor - maps variant types to engine::Mod fields
// ---------------------------------------------------------------------------

struct ModSourceMapper {
    ::engine::Mod& mod;

    bool operator()(const SourceNexus& s) const {
        mod.download_source_type = "nexus";
        mod.download_source_id = std::to_string(s.mod_id);
        mod.download_nxm.file_id = s.file_id;
        mod.download_nxm.nexus_domain = s.game_domain;
        mod.version = s.version;
        if (!s.file_name.empty())
            mod.name = s.file_name;
        return true;
    }

    bool operator()(const SourceLoversLab& s) const {
        mod.download_source_type = "loverslab";
        mod.download_source_id = s.mod_id;
        mod.version = s.version;
        if (!s.file_name.empty())
            mod.name = s.file_name;
        return true;
    }

    bool operator()(const SourceModPub& s) const {
        mod.download_source_type = "modpub";
        mod.download_source_id = s.mod_id;
        mod.version = s.version;
        if (!s.file_name.empty())
            mod.name = s.file_name;
        return true;
    }

    bool operator()(const SourceSteamWorkshop& s) const {
        mod.download_source_type = "steam_workshop";
        mod.download_source_id = std::to_string(s.workshop_item_id);
        mod.version = s.version;
        return true;
    }

    bool operator()(const SourceDirect& s) const {
        mod.download_source_type = "direct";
        mod.download_url = s.url;
        mod.version = s.version;
        if (!s.file_name.empty())
            mod.name = s.file_name;
        return true;
    }
};

// ---------------------------------------------------------------------------
// Resolver::populate_mod_source
// ---------------------------------------------------------------------------

bool Resolver::populate_mod_source(const ModSource& source, ::engine::Mod& mod) {
    return std::visit(ModSourceMapper{mod}, source);
}

// ---------------------------------------------------------------------------
// Rule validation
// ---------------------------------------------------------------------------

std::vector<RuleDiagnostic> Resolver::validate_rules(
    const std::vector<ResolvedMod>& mods,
    const std::vector<Rule>& rules)
{
    std::vector<RuleDiagnostic> diags;

    // Build id set for existence checks.
    std::unordered_set<std::string> mod_ids;
    for (const auto& rm : mods) {
        if (rm.entry) mod_ids.insert(rm.entry->id);
    }

    for (const auto& rule : rules) {
        // "requires" rules: "from" must be present if "to" is present.
        if (rule.type == RuleType::Requires) {
            bool from_present = mod_ids.count(rule.from) > 0;
            bool to_present = mod_ids.count(rule.to) > 0;
            if (to_present && !from_present) {
                diags.push_back({
                    RuleDiagnostic::Severity::Error,
                    "Mod '" + rule.to + "' requires '" + rule.from + "' but it is not in the collection",
                    rule.from,
                    rule.to
                });
            }
        }

        // Warn on references to unknown mod ids.
        if (mod_ids.count(rule.from) == 0) {
            diags.push_back({
                RuleDiagnostic::Severity::Warning,
                "Rule references unknown mod id '" + rule.from + "'",
                rule.from,
                rule.to
            });
        }
        if (mod_ids.count(rule.to) == 0) {
            diags.push_back({
                RuleDiagnostic::Severity::Warning,
                "Rule references unknown mod id '" + rule.to + "'",
                rule.from,
                rule.to
            });
        }
    }

    return diags;
}

// ---------------------------------------------------------------------------
// Resolver::resolve
// ---------------------------------------------------------------------------

ResolvedCollection Resolver::resolve(const Manifest& manifest) const {
    ResolvedCollection result;
    result.mods.reserve(manifest.mods.size());

    // Phase 1: map each ModEntry to a ResolvedMod.
    for (const auto& entry : manifest.mods) {
        ResolvedMod rm;
        rm.entry = &entry;
        rm.mod.id = entry.id;
        rm.mod.name = entry.name;

        if (!populate_mod_source(entry.source, rm.mod)) {
            rm.resolvable = false;
            rm.error = "Unknown source provider";
        }

        result.mods.push_back(std::move(rm));
    }

    // Phase 2: group by phase (stable sort preserving manifest order).
    if (!result.mods.empty()) {
        int max_phase = 0;
        for (const auto& rm : result.mods) {
            if (rm.entry && rm.entry->phase > max_phase)
                max_phase = rm.entry->phase;
        }

        result.phases.resize(static_cast<size_t>(max_phase) + 1);
        for (auto& rm : result.mods) {
            int phase = rm.entry ? rm.entry->phase : 0;
            result.phases[static_cast<size_t>(phase)].push_back(&rm);
        }
    }

    // Phase 3: validate rules.
    result.diagnostics = validate_rules(result.mods, manifest.rules);

    return result;
}

} // namespace engine::Collection
