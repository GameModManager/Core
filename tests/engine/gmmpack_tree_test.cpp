// Tests for the gmmpack tree parser engine module.
//
// Exercises flatten, count, serialize, and validate operations on the
// recursive tree.json structure. The parse_tree() function (JSON -> typed
// structs) is tested via the full round-trip in gmmpack_test.cpp.

#include "engine/gmmpack/tree_parser.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <variant>

namespace gmmpack = engine::gmmpack;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static gmmpack::ModNode make_mod(const std::string &id, bool enabled = true) {
  gmmpack::ModNode m;
  m.id      = id;
  m.enabled = enabled;
  return m;
}

static gmmpack::SeparatorNode make_sep(const std::string &name,
                                       std::vector<gmmpack::TreeNode> children = {},
                                       bool collapsed = false) {
  gmmpack::SeparatorNode s;
  s.name      = name;
  s.collapsed = collapsed;
  s.children  = std::move(children);
  return s;
}

static gmmpack::TreeNode mod(const std::string &id, bool enabled = true) {
  return gmmpack::TreeNode{make_mod(id, enabled)};
}

static gmmpack::TreeNode sep(const std::string &name,
                             std::vector<gmmpack::TreeNode> children = {},
                             bool collapsed                          = false) {
  return gmmpack::TreeNode{make_sep(name, std::move(children), collapsed)};
}

// ---------------------------------------------------------------------------
// PARSE (JSON -> typed structs)
// ---------------------------------------------------------------------------

TEST_CASE("parse empty nodes", "[tree_parser]") {
  auto j    = nlohmann::json::parse(R"({"nodes": []})");
  auto tree = gmmpack::parse_tree(j);
  REQUIRE(tree.nodes.empty());
}

TEST_CASE("parse mods and nested separators", "[tree_parser]") {
  auto j    = nlohmann::json::parse(R"({
        "nodes": [
            {"type": "separator", "name": "Core", "collapsed": false, "children": [
                {"type": "mod", "id": "skyui", "enabled": true},
                {"type": "separator", "name": "ENB", "collapsed": true, "children": [
                    {"type": "mod", "id": "enb-preset", "enabled": false}
                ]}
            ]},
            {"type": "mod", "id": "awesome-mod", "enabled": true}
        ]
    })");
  auto tree = gmmpack::parse_tree(j);
  REQUIRE(tree.nodes.size() == 2);

  auto *sep = std::get_if<gmmpack::SeparatorNode>(&tree.nodes[0].data);
  REQUIRE(sep != nullptr);
  REQUIRE(sep->name == "Core");
  REQUIRE_FALSE(sep->collapsed);
  REQUIRE(sep->children.size() == 2);

  auto *inner = std::get_if<gmmpack::SeparatorNode>(&sep->children[1].data);
  REQUIRE(inner != nullptr);
  REQUIRE(inner->name == "ENB");
  REQUIRE(inner->collapsed);
  REQUIRE(inner->children.size() == 1);
  auto *inner_mod = std::get_if<gmmpack::ModNode>(&inner->children[0].data);
  REQUIRE(inner_mod != nullptr);
  REQUIRE(inner_mod->id == "enb-preset");
  REQUIRE_FALSE(inner_mod->enabled);

  auto *top_mod = std::get_if<gmmpack::ModNode>(&tree.nodes[1].data);
  REQUIRE(top_mod != nullptr);
  REQUIRE(top_mod->id == "awesome-mod");
  REQUIRE(top_mod->enabled);
}

TEST_CASE("parse applies enabled/collapsed defaults", "[tree_parser]") {
  // Schema requires the fields, but the parser stays lenient so a
  // hand-built tree never crashes the engine layer.
  auto j    = nlohmann::json::parse(R"({
        "nodes": [
            {"type": "mod", "id": "bare-mod"},
            {"type": "separator", "name": "Bare", "children": []}
        ]
    })");
  auto tree = gmmpack::parse_tree(j);
  REQUIRE(tree.nodes.size() == 2);
  auto *mod = std::get_if<gmmpack::ModNode>(&tree.nodes[0].data);
  REQUIRE(mod != nullptr);
  REQUIRE(mod->enabled);
  auto *sep = std::get_if<gmmpack::SeparatorNode>(&tree.nodes[1].data);
  REQUIRE(sep != nullptr);
  REQUIRE_FALSE(sep->collapsed);
}

TEST_CASE("parse missing nodes yields empty tree", "[tree_parser]") {
  auto tree = gmmpack::parse_tree(nlohmann::json::object());
  REQUIRE(tree.nodes.empty());
}

// ---------------------------------------------------------------------------
// FLATTEN
// ---------------------------------------------------------------------------

TEST_CASE("flatten empty tree", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  auto flat = gmmpack::flatten_tree(tree);
  REQUIRE(flat.empty());
}

TEST_CASE("flatten single mod", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(mod("skyui"));
  auto flat = gmmpack::flatten_tree(tree);
  REQUIRE(flat.size() == 1);
  REQUIRE(flat[0].id == "skyui");
  REQUIRE(flat[0].enabled);
  REQUIRE(flat[0].separator_path.empty());
}

TEST_CASE("flatten top-level mods preserves order", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(mod("aaa"));
  tree.nodes.push_back(mod("bbb"));
  tree.nodes.push_back(mod("ccc"));
  auto flat = gmmpack::flatten_tree(tree);
  REQUIRE(flat.size() == 3);
  REQUIRE(flat[0].id == "aaa");
  REQUIRE(flat[1].id == "bbb");
  REQUIRE(flat[2].id == "ccc");
}

TEST_CASE("flatten mods inside separator", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(sep("Core", {mod("skyui"), mod("skse")}));
  auto flat = gmmpack::flatten_tree(tree);
  REQUIRE(flat.size() == 2);
  REQUIRE(flat[0].id == "skyui");
  REQUIRE(flat[0].separator_path.size() == 1);
  REQUIRE(flat[0].separator_path[0] == "Core");
  REQUIRE(flat[1].id == "skse");
  REQUIRE(flat[1].separator_path[0] == "Core");
}

TEST_CASE("flatten nested separators", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(
      sep("Root", {mod("a"), sep("Inner", {mod("b"), mod("c")}), mod("d")}));
  auto flat = gmmpack::flatten_tree(tree);
  REQUIRE(flat.size() == 4);

  REQUIRE(flat[0].id == "a");
  REQUIRE(flat[0].separator_path.size() == 1);
  REQUIRE(flat[0].separator_path[0] == "Root");

  REQUIRE(flat[1].id == "b");
  REQUIRE(flat[1].separator_path.size() == 2);
  REQUIRE(flat[1].separator_path[0] == "Root");
  REQUIRE(flat[1].separator_path[1] == "Inner");

  REQUIRE(flat[2].id == "c");
  REQUIRE(flat[2].separator_path.size() == 2);

  REQUIRE(flat[3].id == "d");
  REQUIRE(flat[3].separator_path.size() == 1);
  REQUIRE(flat[3].separator_path[0] == "Root");
}

TEST_CASE("flatten mixed top-level and nested", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(mod("solo"));
  tree.nodes.push_back(sep("Group", {mod("inside")}));
  tree.nodes.push_back(mod("bottom"));
  auto flat = gmmpack::flatten_tree(tree);
  REQUIRE(flat.size() == 3);
  REQUIRE(flat[0].id == "solo");
  REQUIRE(flat[0].separator_path.empty());
  REQUIRE(flat[1].id == "inside");
  REQUIRE(flat[1].separator_path.size() == 1);
  REQUIRE(flat[2].id == "bottom");
  REQUIRE(flat[2].separator_path.empty());
}

TEST_CASE("flatten respects enabled flag", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(mod("on", true));
  tree.nodes.push_back(mod("off", false));
  auto flat = gmmpack::flatten_tree(tree);
  REQUIRE(flat[0].enabled);
  REQUIRE_FALSE(flat[1].enabled);
}

TEST_CASE("flatten is the conflict resolution priority order", "[tree_parser]") {
  // Spec: "flattened top-to-bottom sequence IS the conflict resolution
  // priority order (MO2-style)" -- first mod in the flat list wins.
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(sep("A", {mod("winner"), mod("second")}));
  tree.nodes.push_back(sep("B", {mod("third")}));
  tree.nodes.push_back(mod("last"));
  auto flat = gmmpack::flatten_tree(tree);
  REQUIRE(flat[0].id == "winner");
  REQUIRE(flat[1].id == "second");
  REQUIRE(flat[2].id == "third");
  REQUIRE(flat[3].id == "last");
}

// ---------------------------------------------------------------------------
// COUNT
// ---------------------------------------------------------------------------

TEST_CASE("count empty tree", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  REQUIRE(gmmpack::count_tree_mods(tree) == 0);
  REQUIRE(gmmpack::count_tree_separators(tree) == 0);
}

TEST_CASE("count mods and separators", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(
      sep("Root", {mod("a"), sep("Inner", {mod("b"), mod("c")}), mod("d")}));
  tree.nodes.push_back(mod("e"));
  REQUIRE(gmmpack::count_tree_mods(tree) == 5);
  REQUIRE(gmmpack::count_tree_separators(tree) == 2);
}

TEST_CASE("count separators without mods", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(sep("Empty"));
  tree.nodes.push_back(sep("AlsoEmpty"));
  REQUIRE(gmmpack::count_tree_mods(tree) == 0);
  REQUIRE(gmmpack::count_tree_separators(tree) == 2);
}

// ---------------------------------------------------------------------------
// SERIALIZE
// ---------------------------------------------------------------------------

TEST_CASE("serialize empty tree", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  auto j = gmmpack::serialize_tree(tree);
  REQUIRE(j.contains("nodes"));
  REQUIRE(j["nodes"].empty());
}

TEST_CASE("serialize single mod node", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(mod("skyui"));
  auto j = gmmpack::serialize_tree(tree);
  REQUIRE(j["nodes"].size() == 1);
  REQUIRE(j["nodes"][0]["type"] == "mod");
  REQUIRE(j["nodes"][0]["id"] == "skyui");
  REQUIRE(j["nodes"][0]["enabled"] == true);
}

TEST_CASE("serialize separator with children", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(sep("Core", {mod("a"), mod("b")}));
  auto j = gmmpack::serialize_tree(tree);
  REQUIRE(j["nodes"].size() == 1);
  auto &sep_node = j["nodes"][0];
  REQUIRE(sep_node["type"] == "separator");
  REQUIRE(sep_node["name"] == "Core");
  REQUIRE(sep_node["collapsed"] == false);
  REQUIRE(sep_node["children"].size() == 2);
  REQUIRE(sep_node["children"][0]["id"] == "a");
  REQUIRE(sep_node["children"][1]["id"] == "b");
}

TEST_CASE("serialize nested separator structure", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(sep("Outer", {mod("x"), sep("Inner", {mod("y")})}, true));
  auto j      = gmmpack::serialize_tree(tree);
  auto &outer = j["nodes"][0];
  REQUIRE(outer["collapsed"] == true);
  REQUIRE(outer["children"].size() == 2);
  auto &inner = outer["children"][1];
  REQUIRE(inner["type"] == "separator");
  REQUIRE(inner["name"] == "Inner");
  REQUIRE(inner["children"].size() == 1);
  REQUIRE(inner["children"][0]["id"] == "y");
}

TEST_CASE("serialize preserves disabled mod", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(mod("disabled_mod", false));
  auto j = gmmpack::serialize_tree(tree);
  REQUIRE(j["nodes"][0]["enabled"] == false);
}

// ---------------------------------------------------------------------------
// SERIALIZE round-trip: parse_tree -> serialize -> parse_tree
// ---------------------------------------------------------------------------

TEST_CASE("round-trip: serialize then parse produces same tree", "[tree_parser]") {
  gmmpack::TreeRoot original;
  original.nodes.push_back(sep(
      "Group", {mod("a"), sep("Inner", {mod("b"), mod("c", false)}), mod("d")}, true));
  original.nodes.push_back(mod("e"));

  auto j             = gmmpack::serialize_tree(original);
  auto round_tripped = gmmpack::parse_tree(j);

  // Compare mod count
  REQUIRE(gmmpack::count_tree_mods(round_tripped) ==
          gmmpack::count_tree_mods(original));

  // Compare flatten order + enabled flags
  auto flat_orig = gmmpack::flatten_tree(original);
  auto flat_rt   = gmmpack::flatten_tree(round_tripped);
  REQUIRE(flat_orig.size() == flat_rt.size());
  for (size_t i = 0; i < flat_orig.size(); ++i) {
    REQUIRE(flat_orig[i].id == flat_rt[i].id);
    REQUIRE(flat_orig[i].enabled == flat_rt[i].enabled);
    REQUIRE(flat_orig[i].separator_path == flat_rt[i].separator_path);
  }
}

// ---------------------------------------------------------------------------
// VALIDATE
// ---------------------------------------------------------------------------

TEST_CASE("validate empty tree passes", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  auto diag = gmmpack::validate_tree(tree);
  REQUIRE(diag.empty());
}

TEST_CASE("validate valid tree passes", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(sep("Core", {mod("skyui"), sep("ENB", {mod("enb-preset")})}));
  tree.nodes.push_back(mod("awesome-mod"));
  auto diag = gmmpack::validate_tree(tree);
  REQUIRE(diag.empty());
}

TEST_CASE("validate rejects empty separator name", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(sep("", {mod("a")}));
  auto diag = gmmpack::validate_tree(tree);
  REQUIRE_FALSE(diag.empty());
  bool found = false;
  for (const auto &d : diag) {
    if (d.message.find("separator name") != std::string::npos) {
      found = true;
      break;
    }
  }
  REQUIRE(found);
}

TEST_CASE("validate rejects empty mod id", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(mod(""));
  auto diag = gmmpack::validate_tree(tree);
  REQUIRE_FALSE(diag.empty());
  bool found = false;
  for (const auto &d : diag) {
    if (d.message.find("mod id") != std::string::npos) {
      found = true;
      break;
    }
  }
  REQUIRE(found);
}

TEST_CASE("validate rejects duplicate mod ids", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(sep("A", {mod("shared"), mod("other")}));
  tree.nodes.push_back(sep("B", {mod("shared")}));
  auto diag = gmmpack::validate_tree(tree);
  REQUIRE_FALSE(diag.empty());
  bool found = false;
  for (const auto &d : diag) {
    if (d.message.find("duplicate") != std::string::npos) {
      found = true;
      break;
    }
  }
  REQUIRE(found);
}

TEST_CASE("validate passes with single mod", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(mod("unique-mod"));
  auto diag = gmmpack::validate_tree(tree);
  REQUIRE(diag.empty());
}

TEST_CASE("validate reports multiple errors", "[tree_parser]") {
  gmmpack::TreeRoot tree;
  tree.nodes.push_back(mod(""));
  tree.nodes.push_back(sep(""));
  tree.nodes.push_back(mod(""));
  auto diag = gmmpack::validate_tree(tree);
  // At least: empty mod id, empty separator name, empty mod id
  REQUIRE(diag.size() >= 3);
}
