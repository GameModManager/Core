// Engine test for the deploy-space classification of conflict-registry paths
// (root_override). A mod flagged [General] rootOverride deploys to the game
// root; a leading <deploy_prefix>/ segment inside it is still data content and
// must be shown/stripped in the Data view, everything else belongs to the Root
// view. Non-root-flagged mods never change view.
#include "engine/deploy/root_override.h"

#include <cstdio>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using engine::classify_registry_path;
using engine::DeploySpace;

static int failures = 0;
static int passes = 0;
static void check(bool cond, const char* what) {
    std::printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
    if (cond)
        ++passes;
    else
        ++failures;
}

using Owners = std::vector<std::pair<std::string, int>>;

int main() {
    const std::unordered_set<std::string> no_root = {};
    const std::unordered_set<std::string> with_root = {"SKSE"};

    // No root-flagged owner: everything stays in the Data view, path as-is.
    auto flat = classify_registry_path(
        "meshes/test.nif", Owners{{"ModA", 1}, {"ModB", 2}}, no_root, "Data");
    check(flat.space == DeploySpace::Data, "no root owner -> Data space");
    check(flat.display_path == "meshes/test.nif",
          "no root owner -> path unchanged");

    // Root-flagged owner + Data/ prefix -> Data space, prefix stripped.
    auto data = classify_registry_path(
        "Data/SKSE/Plugins/foo.dll", Owners{{"SKSE", 5}}, with_root, "Data");
    check(data.space == DeploySpace::Data,
          "root owner + Data/ prefix -> Data space");
    check(data.display_path == "SKSE/Plugins/foo.dll",
          "root owner + Data/ prefix -> prefix stripped");

    // Root-flagged owner, no prefix -> Root space, path as-is.
    auto root = classify_registry_path(
        "skse64_loader.exe", Owners{{"SKSE", 5}}, with_root, "Data");
    check(root.space == DeploySpace::Root,
          "root owner, no prefix -> Root space");
    check(root.display_path == "skse64_loader.exe",
          "root owner, no prefix -> path unchanged");

    // Exact "Data" alone -> Data space with an empty display path.
    auto exact = classify_registry_path("Data", Owners{{"SKSE", 5}}, with_root, "Data");
    check(exact.space == DeploySpace::Data, "root owner, exact prefix -> Data space");
    check(exact.display_path.empty(),
          "root owner, exact prefix -> empty display path");

    // Case-insensitive prefix segment (Windows game dirs).
    auto ci = classify_registry_path(
        "data/skse64_loader.exe", Owners{{"SKSE", 5}}, with_root, "Data");
    check(ci.space == DeploySpace::Data, "prefix match is case-insensitive");
    check(ci.display_path == "skse64_loader.exe",
          "case-insensitive strip keeps the rest");

    // A look-alike prefix (Datax) must not be stripped.
    auto lookalike = classify_registry_path(
        "Datax/tool.dll", Owners{{"SKSE", 5}}, with_root, "Data");
    check(lookalike.space == DeploySpace::Root,
          "Datax is not the Data prefix -> Root space");

    // One root-flagged owner among several is enough.
    auto mixed = classify_registry_path(
        "ControlMap_Custom.txt", Owners{{"ModA", 1}, {"SKSE", 5}}, with_root, "Data");
    check(mixed.space == DeploySpace::Root,
          "any root owner -> Root space");

    // Empty deploy_prefix (no data-dir concept): a root owner's content is all
    // root content; a non-root owner still stays in the Data view.
    auto no_prefix = classify_registry_path(
        "resources/gfx/a.png", Owners{{"SKSE", 5}}, with_root, "");
    check(no_prefix.space == DeploySpace::Root,
          "empty deploy_prefix -> Root space for a root owner");
    auto no_prefix_flat = classify_registry_path(
        "resources/gfx/a.png", Owners{{"ModA", 1}}, no_root, "");
    check(no_prefix_flat.space == DeploySpace::Data,
          "empty deploy_prefix, no root owner -> Data space");

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
