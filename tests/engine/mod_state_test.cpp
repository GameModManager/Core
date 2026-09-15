// Engine test for per-mod instance state tracking.
//
// Covers all 4 tickets:
// - Workspace-b1sb: install order + timestamp
// - Workspace-1spd: position, nesting, hierarchy, separator visuals
// - Workspace-ofzq: hidden/disabled state per mod and per file
// - Workspace-0tsl: deployAtRoot flag per mod
//
// Round-trip through mod_state.json, atomic write, missing file handling.

#include "engine/core/instance/mod_state.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {
void require(bool cond, const char* msg) {
    INFO(msg);
    REQUIRE(cond);
}
}

static fs::path test_root(const char* name) {
    return fs::path("/tmp/gmm_mod_state_test") / name;
}

TEST_CASE("mod state - install order", "[engine]") {
    using engine::ModStateTracker;

    const fs::path root = test_root("install_order");
    fs::remove_all(root);
    fs::create_directories(root);

    ModStateTracker tracker(root);

    // Initial load of missing file succeeds.
    require(tracker.load(), "load missing file succeeds");

    // Record installs for three mods.
    tracker.record_install("mod_a");
    auto& a = tracker.entry("mod_a");
    require(a.install_order == 1, "mod_a gets order 1");
    require(a.installed_at > 0, "mod_a gets a timestamp");

    tracker.record_install("mod_b");
    tracker.record_install("mod_c");
    auto& b = tracker.entry("mod_b");
    auto& c = tracker.entry("mod_c");
    require(b.install_order == 2, "mod_b gets order 2");
    require(c.install_order == 3, "mod_c gets order 3");

    // Reinstall updates order and timestamp.
    int64_t old_at = a.installed_at;
    tracker.record_install("mod_a");
    auto& a2 = tracker.entry("mod_a");
    require(a2.install_order == 4, "reinstall bumps order past max");
    require(a2.installed_at >= old_at, "reinstall updates timestamp");

    // Save + load round-trip.
    require(tracker.save(), "save succeeds");
    ModStateTracker reload(root);
    require(reload.load(), "reload succeeds");
    auto& ra = reload.entry("mod_a");
    require(ra.install_order == 4, "install_order roundtrips");
    require(ra.installed_at == a2.installed_at, "installed_at roundtrips");

    // next_install_order reflects the max.
    require(reload.next_install_order() == 5, "next_install_order is max+1");

    fs::remove_all(root);
}

TEST_CASE("mod state - position and hierarchy", "[engine]") {
    using engine::ModStateTracker;

    const fs::path root = test_root("position");
    fs::remove_all(root);
    fs::create_directories(root);

    ModStateTracker tracker(root);

    // Top-level mod.
    tracker.set_position("mod_a", 0);
    tracker.set_nesting("mod_a", "", 0);
    auto& a = tracker.entry("mod_a");
    require(a.list_position == 0, "mod_a position set");
    require(a.parent_separator.empty(), "mod_a has no parent");
    require(a.depth == 0, "mod_a is top-level");

    // Nested mod under a separator.
    tracker.set_position("mod_b", 1);
    tracker.set_nesting("mod_b", "Animals", 1);
    auto& b = tracker.entry("mod_b");
    require(b.list_position == 1, "mod_b position set");
    require(b.parent_separator == "Animals", "mod_b parent is Animals");
    require(b.depth == 1, "mod_b depth is 1");

    // Separator itself with visual props.
    tracker.set_separator_visual("Animals", "#888888", true);
    auto& sep = tracker.entry("Animals");
    require(sep.separator_color == "#888888", "separator color set");
    require(sep.collapsed == true, "separator collapsed");

    // Deeply nested mod.
    tracker.set_nesting("mod_c", "Textures", 2);
    auto& c = tracker.entry("mod_c");
    require(c.depth == 2, "mod_c depth is 2");
    require(c.parent_separator == "Textures", "mod_c parent is Textures");

    // Save + roundtrip.
    require(tracker.save(), "save succeeds");
    ModStateTracker reload(root);
    require(reload.load(), "reload succeeds");
    auto& rb = reload.entry("mod_b");
    require(rb.parent_separator == "Animals", "parent_separator roundtrips");
    auto& rsep = reload.entry("Animals");
    require(rsep.separator_color == "#888888", "separator color roundtrips");
    require(rsep.collapsed == true, "collapsed roundtrips");
    auto& rc = reload.entry("mod_c");
    require(rc.depth == 2, "depth roundtrips");

    fs::remove_all(root);
}

TEST_CASE("mod state - hidden and disabled", "[engine]") {
    using engine::ModStateTracker;

    const fs::path root = test_root("hidden_disabled");
    fs::remove_all(root);
    fs::create_directories(root);

    ModStateTracker tracker(root);

    // Hidden mod.
    tracker.set_hidden("mod_hidden", true);
    auto& h = tracker.entry("mod_hidden");
    require(h.hidden == true, "mod_hidden is hidden");
    require(h.disabled == false, "hidden mod not disabled by default");

    // Disabled mod.
    tracker.set_disabled("mod_disabled", true);
    auto& d = tracker.entry("mod_disabled");
    require(d.disabled == true, "mod_disabled is disabled");

    // Hidden files within a mod.
    std::vector<std::string> hidden = {"meshes/secret.nif", "textures/hidden.dds"};
    tracker.set_hidden_files("mod_with_hiddens", hidden);
    auto& wh = tracker.entry("mod_with_hiddens");
    require(wh.hidden_files.size() == 2, "two hidden files");
    require(wh.hidden_files[0] == "meshes/secret.nif",
            "first hidden file correct");
    require(wh.hidden_files[1] == "textures/hidden.dds",
            "second hidden file correct");

    // Toggle off.
    tracker.set_hidden("mod_hidden", false);
    require(tracker.entry("mod_hidden").hidden == false,
            "hidden toggled off");

    // Save + roundtrip.
    require(tracker.save(), "save succeeds");
    ModStateTracker reload(root);
    require(reload.load(), "reload succeeds");
    auto& rd = reload.entry("mod_disabled");
    require(rd.disabled == true, "disabled roundtrips");
    auto& rwh = reload.entry("mod_with_hiddens");
    require(rwh.hidden_files.size() == 2, "hidden_files roundtrips");

    fs::remove_all(root);
}

TEST_CASE("mod state - deploy at root", "[engine]") {
    using engine::ModStateTracker;

    const fs::path root = test_root("deploy_at_root");
    fs::remove_all(root);
    fs::create_directories(root);

    ModStateTracker tracker(root);

    // Default is false.
    tracker.set_position("mod_a", 0);
    require(tracker.entry("mod_a").deploy_at_root == false,
            "deploy_at_root defaults to false");

    // Set flag.
    tracker.set_deploy_at_root("mod_a", true);
    require(tracker.entry("mod_a").deploy_at_root == true,
            "deploy_at_root set to true");

    // Another mod stays false.
    tracker.set_position("mod_b", 1);
    require(tracker.entry("mod_b").deploy_at_root == false,
            "other mod unaffected");

    // Clear flag.
    tracker.set_deploy_at_root("mod_a", false);
    require(tracker.entry("mod_a").deploy_at_root == false,
            "deploy_at_root cleared");

    // Save + roundtrip.
    tracker.set_deploy_at_root("mod_a", true);
    require(tracker.save(), "save succeeds");
    ModStateTracker reload(root);
    require(reload.load(), "reload succeeds");
    auto& ra = reload.entry("mod_a");
    require(ra.deploy_at_root == true, "deploy_at_root roundtrips");

    fs::remove_all(root);
}

TEST_CASE("mod state - comprehensive roundtrip", "[engine]") {
    using engine::ModStateTracker;

    const fs::path root = test_root("comprehensive");
    fs::remove_all(root);
    fs::create_directories(root);

    ModStateTracker tracker(root);

    // Build a realistic state: 3 mods + 1 separator.
    tracker.record_install("SKSE");
    tracker.set_position("SKSE", 0);
    tracker.set_nesting("SKSE", "", 0);

    tracker.record_install("USSEP");
    tracker.set_position("USSEP", 1);
    tracker.set_nesting("USSEP", "", 0);

    tracker.set_separator_visual("Graphics", "#ff0000", false);
    tracker.set_position("Graphics", 2);
    tracker.set_nesting("Graphics", "", 0);

    tracker.record_install("ENB");
    tracker.set_position("ENB", 3);
    tracker.set_nesting("ENB", "Graphics", 1);
    tracker.set_deploy_at_root("ENB", true);

    tracker.set_hidden("SKSE", true);
    tracker.set_disabled("USSEP", true);
    tracker.set_hidden_files("ENB", {"d3d11.dll"});

    // Save.
    require(tracker.save(), "save succeeds");

    // Verify file exists.
    require(fs::exists(root / "mod_state.json"),
            "mod_state.json created");

    // Load into fresh tracker.
    ModStateTracker reload(root);
    require(reload.load(), "reload succeeds");
    require(reload.all_entries().size() == 4, "all 4 entries loaded");

    // Verify all fields.
    auto& skse = reload.entry("SKSE");
    require(skse.install_order == 1, "SKSE order");
    require(skse.hidden == true, "SKSE hidden");
    require(skse.depth == 0, "SKSE top-level");

    auto& ussep = reload.entry("USSEP");
    require(ussep.disabled == true, "USSEP disabled");

    auto& sep = reload.entry("Graphics");
    require(sep.separator_color == "#ff0000", "separator color");
    require(sep.collapsed == false, "separator not collapsed");

    auto& enb = reload.entry("ENB");
    require(enb.parent_separator == "Graphics", "ENB parent");
    require(enb.depth == 1, "ENB nested");
    require(enb.deploy_at_root == true, "ENB deploy_at_root");
    require(enb.hidden_files.size() == 1, "ENB hidden file count");
    require(enb.hidden_files[0] == "d3d11.dll", "ENB hidden file");

    fs::remove_all(root);
}

TEST_CASE("mod state - remove entry", "[engine]") {
    using engine::ModStateTracker;

    const fs::path root = test_root("remove");
    fs::remove_all(root);
    fs::create_directories(root);

    ModStateTracker tracker(root);
    tracker.record_install("mod_a");
    tracker.record_install("mod_b");
    require(tracker.all_entries().size() == 2, "two entries");

    tracker.remove("mod_a");
    require(tracker.all_entries().size() == 1, "one entry after remove");
    require(tracker.all_entries().count("mod_a") == 0, "mod_a gone");
    require(tracker.all_entries().count("mod_b") == 1, "mod_b still present");

    // Remove non-existent is a no-op.
    tracker.remove("nonexistent");
    require(tracker.all_entries().size() == 1, "no-op remove");

    // Roundtrip preserves removal.
    require(tracker.save(), "save succeeds");
    ModStateTracker reload(root);
    require(reload.load(), "reload succeeds");
    require(reload.all_entries().size() == 1, "one entry after roundtrip");
    require(reload.all_entries().count("mod_a") == 0, "mod_a still gone");

    fs::remove_all(root);
}

TEST_CASE("mod state - set_all_entries", "[engine]") {
    using engine::ModStateTracker;
    using engine::ModTrackingEntry;

    const fs::path root = test_root("bulk");
    fs::remove_all(root);
    fs::create_directories(root);

    ModStateTracker tracker(root);

    std::unordered_map<std::string, ModTrackingEntry> bulk;
    ModTrackingEntry e1;
    e1.install_order = 10;
    e1.deploy_at_root = true;
    bulk["mod_x"] = e1;

    ModTrackingEntry e2;
    e2.hidden = true;
    e2.hidden_files = {"a.txt", "b.txt"};
    bulk["mod_y"] = e2;

    tracker.set_all_entries(std::move(bulk));
    require(tracker.all_entries().size() == 2, "bulk set");
    auto& mx = tracker.entry("mod_x");
    require(mx.deploy_at_root == true, "bulk mod_x");
    auto& my = tracker.entry("mod_y");
    require(my.hidden == true, "bulk mod_y");

    fs::remove_all(root);
}

TEST_CASE("mod state - invalid JSON fails gracefully", "[engine]") {
    using engine::ModStateTracker;

    const fs::path root = test_root("empty_file");
    fs::remove_all(root);
    fs::create_directories(root);

    // Write invalid JSON.
    {
        std::ofstream out(root / "mod_state.json");
        out << "not json at all";
    }

    ModStateTracker tracker(root);
    require(!tracker.load(), "invalid JSON fails to load");

    // Corrupt JSON: still fails gracefully.
    {
        std::ofstream out(root / "mod_state.json");
        out << "{";
    }
    ModStateTracker tracker2(root);
    require(!tracker2.load(), "truncated JSON fails to load");

    fs::remove_all(root);
}

TEST_CASE("mod state - hidden_files edge cases", "[engine]") {
    using engine::ModStateTracker;

    const fs::path root = test_root("hidden_files_edge");
    fs::remove_all(root);
    fs::create_directories(root);

    ModStateTracker tracker(root);

    // Empty hidden_files vector.
    tracker.set_hidden_files("mod_a", {});
    require(tracker.entry("mod_a").hidden_files.empty(),
            "empty hidden_files vector");

    // Save + roundtrip: empty vector stays empty.
    require(tracker.save(), "save succeeds");
    ModStateTracker reload(root);
    require(reload.load(), "reload succeeds");
    auto& ra = reload.entry("mod_a");
    require(ra.hidden_files.empty(),
            "empty hidden_files stays empty");

    fs::remove_all(root);
}
