#pragma once

#include <cstdint>
#include <functional>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace YAML { class Node; }

namespace engine {

// Isaac auto-sort engine.
//
// Reads masterlist.yaml (community dependency rules) + user_rules.yaml
// (personal overrides), matches mods by workshop ID / pattern / metadata tag,
// classifies them into priority groups, then topologically sorts within each
// group by before/after dependency constraints.
//
// Result is a list of mod folder names in the correct load order (top = loads first).

struct SortGroup {
    std::string name;
    int priority = 99;
};

struct ModEntry {
    int64_t workshop_id = 0;        // Steam Workshop ID (from folder name or masterlist)
    std::string name;               // human-readable label
    std::string group = "unknown";  // group name (from masterlist)
    std::vector<int64_t> after;     // must load after these Workshop IDs
    std::vector<int64_t> before;    // must load before these Workshop IDs
    std::vector<int64_t> required_by;  // warns if these are missing
    std::string pattern;            // regex matched against folder name
    std::string tag;                // matched against metadata.xml <tag id="...">
    bool preserve_name = false;     // don't auto-rename this mod
};

struct SortModInfo {
    std::string folder_name;
    std::string display_name;
    int64_t workshop_id = 0;
    std::vector<std::string> tags;  // from metadata.xml
};

class IsaacAutoSorter {
public:
    // Load masterlist from YAML text
    void load_masterlist(const std::string& yaml_text);

    // Load user rules from YAML text
    void load_user_rules(const std::string& yaml_text);

    // Set bundled masterlist as fallback
    void set_bundled_masterlist(const std::string& yaml_text);

    // Sort mods according to masterlist + user rules.
    // Returns ordered list of folder names (top = loads first = wins conflicts).
    std::vector<std::string> auto_sort(const std::vector<SortModInfo>& mods) const;

    // Check if a mod should preserve its folder name (not auto-renamed)
    [[nodiscard]] bool should_preserve_name(int64_t workshop_id) const;

    // Get the group name for a workshop ID (or "unknown")
    [[nodiscard]] std::string group_for(int64_t workshop_id) const;

    // Access loaded data
    [[nodiscard]] const std::vector<SortGroup>& groups() const { return groups_; }
    [[nodiscard]] const std::vector<ModEntry>& mod_entries() const { return mod_entries_; }

private:
    // Match a mod against the masterlist entries
    const ModEntry* match_mod(const SortModInfo& mod) const;

    // Topological sort within a group
    std::vector<std::string> topological_sort(
        const std::vector<SortModInfo>& group_mods,
        const std::unordered_map<int64_t, const ModEntry*>& mod_lookup) const;

    // Extract workshop ID from folder name (regex: _(\\d+)$)
    static int64_t extract_workshop_id(const std::string& folder_name);

    // YAML parser using yaml-cpp
    static void parse_masterlist_yaml(const YAML::Node& root,
                                       std::vector<SortGroup>& groups,
                                       std::vector<ModEntry>& mods);

    std::vector<SortGroup> groups_;
    std::vector<ModEntry> mod_entries_;
    std::vector<ModEntry> user_entries_;  // merged on top of mod_entries_
};

}  // namespace engine
