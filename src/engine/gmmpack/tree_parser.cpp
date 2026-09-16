#include "engine/gmmpack/tree_parser.h"

#include <algorithm>
#include <unordered_set>

namespace engine::gmmpack {

// ---------------------------------------------------------------------------
// Parse (JSON -> typed structs, recursive)
// ---------------------------------------------------------------------------

static TreeNode parse_tree_node(const nlohmann::json& j) {
    std::string type = j.value("type", "");
    if (type == "separator") {
        SeparatorNode sep;
        sep.name = j.value("name", "");
        sep.collapsed = j.value("collapsed", false);
        if (j.contains("children")) {
            for (const auto& child : j["children"]) {
                sep.children.push_back(parse_tree_node(child));
            }
        }
        return TreeNode{sep};
    }
    ModNode mod;
    mod.id = j.value("id", "");
    mod.enabled = j.value("enabled", true);
    return TreeNode{mod};
}

TreeRoot parse_tree(const nlohmann::json& j) {
    TreeRoot tree;
    if (j.contains("nodes")) {
        for (const auto& node : j["nodes"]) {
            tree.nodes.push_back(parse_tree_node(node));
        }
    }
    return tree;
}

// ---------------------------------------------------------------------------
// Flatten (recursive)
// ---------------------------------------------------------------------------

static void flatten_nodes(const std::vector<TreeNode>& nodes,
                          std::vector<std::string>& path,
                          std::vector<FlattenedMod>& out) {
    for (const auto& node : nodes) {
        if (auto* sep = std::get_if<SeparatorNode>(&node.data)) {
            path.push_back(sep->name);
            flatten_nodes(sep->children, path, out);
            path.pop_back();
        } else if (auto* mod = std::get_if<ModNode>(&node.data)) {
            out.push_back({mod->id, mod->enabled, path});
        }
    }
}

std::vector<FlattenedMod> flatten_tree(const TreeRoot& tree) {
    std::vector<FlattenedMod> result;
    std::vector<std::string> path;
    flatten_nodes(tree.nodes, path, result);
    return result;
}

// ---------------------------------------------------------------------------
// Count (recursive)
// ---------------------------------------------------------------------------

static void count_nodes(const std::vector<TreeNode>& nodes,
                        std::size_t& mods, std::size_t& seps) {
    for (const auto& node : nodes) {
        if (auto* sep = std::get_if<SeparatorNode>(&node.data)) {
            ++seps;
            count_nodes(sep->children, mods, seps);
        } else if (std::get_if<ModNode>(&node.data)) {
            ++mods;
        }
    }
}

std::size_t count_tree_mods(const TreeRoot& tree) {
    std::size_t mods = 0, seps = 0;
    count_nodes(tree.nodes, mods, seps);
    return mods;
}

std::size_t count_tree_separators(const TreeRoot& tree) {
    std::size_t mods = 0, seps = 0;
    count_nodes(tree.nodes, mods, seps);
    return seps;
}

// ---------------------------------------------------------------------------
// Serialize
// ---------------------------------------------------------------------------

static nlohmann::json serialize_node(const TreeNode& node) {
    if (auto* sep = std::get_if<SeparatorNode>(&node.data)) {
        nlohmann::json j;
        j["type"] = "separator";
        j["name"] = sep->name;
        j["collapsed"] = sep->collapsed;
        j["children"] = nlohmann::json::array();
        for (const auto& child : sep->children) {
            j["children"].push_back(serialize_node(child));
        }
        return j;
    }
    if (auto* mod = std::get_if<ModNode>(&node.data)) {
        return {{"type", "mod"}, {"id", mod->id}, {"enabled", mod->enabled}};
    }
    return nullptr;
}

nlohmann::json serialize_tree(const TreeRoot& tree) {
    nlohmann::json j;
    j["nodes"] = nlohmann::json::array();
    for (const auto& node : tree.nodes) {
        j["nodes"].push_back(serialize_node(node));
    }
    return j;
}

// ---------------------------------------------------------------------------
// Validate
// ---------------------------------------------------------------------------

static void validate_nodes(const std::vector<TreeNode>& nodes,
                           const std::string& prefix,
                           std::unordered_set<std::string>& seen_ids,
                           Diagnostics& diag) {
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        std::string loc = prefix + "nodes[" + std::to_string(i) + "]";

        if (auto* sep = std::get_if<SeparatorNode>(&node.data)) {
            if (sep->name.empty()) {
                diag.push_back({Diagnostic::Severity::Error, loc + ".name",
                                "separator name must not be empty"});
            }
            validate_nodes(sep->children, loc + ".children.", seen_ids, diag);
        } else if (auto* mod = std::get_if<ModNode>(&node.data)) {
            if (mod->id.empty()) {
                diag.push_back({Diagnostic::Severity::Error, loc + ".id",
                                "mod id must not be empty"});
            }
            if (!seen_ids.insert(mod->id).second) {
                diag.push_back({Diagnostic::Severity::Error, loc + ".id",
                                "duplicate mod id: " + mod->id});
            }
        } else {
            diag.push_back(
                {Diagnostic::Severity::Error, loc, "unknown node type"});
        }
    }
}

Diagnostics validate_tree(const TreeRoot& tree) {
    Diagnostics diag;
    std::unordered_set<std::string> seen_ids;
    validate_nodes(tree.nodes, "", seen_ids, diag);
    return diag;
}

}  // namespace engine::gmmpack
