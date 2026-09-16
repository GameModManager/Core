// Test for the engine::Pack adapter interface: source routing, the
// Outcome carrier, the Registry, and the full installer-facing contract
// driven through a fake in-memory adapter (one per source kind: file-based
// .gmmpack and API-based nxm:// collection).
#include "engine/pack/adapter.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {

// Minimal in-memory adapter: serves a fixed pack without touching disk or
// network. The file flavor accepts .gmmpack paths, the API flavor nxm:// URLs
// (Nexus collections resolve over the API, never as raw files).
class FakeAdapter : public engine::Pack::Interface {
public:
    explicit FakeAdapter(std::string format, engine::Pack::SourceKind kind)
        : format_(std::move(format)), kind_(kind) {}

    [[nodiscard]] std::string format_id() const override { return format_; }

    [[nodiscard]] bool can_handle_source(
        const std::string& url_or_path) const override {
        return engine::Pack::detect_source_kind(url_or_path) == kind_;
    }

    [[nodiscard]] engine::Pack::Outcome<engine::Pack::PackManifest> resolve_pack(
        const std::string& url_or_path) override {
        if (!can_handle_source(url_or_path)) {
            return engine::Pack::Outcome<engine::Pack::PackManifest>::failure(
                "unsupported source: " + url_or_path);
        }
        manifest_.name = "Fake Pack";
        manifest_.author = "tester";
        manifest_.game_id = "skyrimspecialedition";
        manifest_.revision = "3";
        manifest_.mods = {
            {"aaa", "Mod A", "1.0", "nexus", "100", "1000", false},
            {"bbb", "Mod B", "", "direct", "https://example.com/b.zip", "", true},
        };
        tree_ = {
            {"sep", "", "Essentials", true},
            {"aaa", "sep", "Mod A", false},
        };
        rules_ = {{engine::Pack::Rule::Type::After, "bbb", "aaa", ""}};
        patches_ = {{"p1", "meshes/a.nif", "patches/p1.bin"}};
        ini_edits_ = {{"Skyrim.ini", "General", "bEnable", "1"}};
        executables_ = {{"skse", "SKSE", "skse64_loader.exe", "-forcesteamloader"}};
        choices_ = {{"c1",
                     "Textures",
                     engine::Pack::ChoiceGroup::Mode::ExactlyOne,
                     {"1K", "2K"},
                     "2K"}};
        resolved_ = true;
        return engine::Pack::Outcome<engine::Pack::PackManifest>::success(
            manifest_);
    }

    [[nodiscard]] engine::Pack::Outcome<engine::Pack::ResolvedMod> resolve_mod(
        const engine::Pack::ModEntry& entry) override {
        if (entry.id.empty()) {
            return engine::Pack::Outcome<engine::Pack::ResolvedMod>::failure(
                "empty entry id");
        }
        engine::Pack::ResolvedMod r;
        r.entry_id = entry.id;
        r.display_name = entry.name;
        r.archive_name = entry.id + ".zip";
        r.source_type = entry.source_type;
        r.source_id = entry.source_id;
        r.file_id = entry.file_id;
        return engine::Pack::Outcome<engine::Pack::ResolvedMod>::success(r);
    }

    [[nodiscard]] engine::Pack::DownloadInfo get_download_info(
        const engine::Pack::ResolvedMod& resolved) const override {
        return {resolved.archive_name, resolved.display_name, "", 0};
    }

    [[nodiscard]] std::vector<engine::Pack::TreeNode> get_tree() const override {
        return resolved_ ? tree_ : std::vector<engine::Pack::TreeNode>{};
    }
    [[nodiscard]] std::vector<engine::Pack::Rule> get_rules() const override {
        return resolved_ ? rules_ : std::vector<engine::Pack::Rule>{};
    }
    [[nodiscard]] std::vector<engine::Pack::Patch> get_patches() const override {
        return resolved_ ? patches_ : std::vector<engine::Pack::Patch>{};
    }
    [[nodiscard]] std::vector<engine::Pack::IniEdit> get_ini_edits()
        const override {
        return resolved_ ? ini_edits_ : std::vector<engine::Pack::IniEdit>{};
    }
    [[nodiscard]] std::vector<engine::Pack::ExecEntry> get_executables()
        const override {
        return resolved_ ? executables_
                         : std::vector<engine::Pack::ExecEntry>{};
    }
    [[nodiscard]] std::vector<engine::Pack::ChoiceGroup> get_choice_groups()
        const override {
        return resolved_ ? choices_ : std::vector<engine::Pack::ChoiceGroup>{};
    }

private:
    std::string format_;
    engine::Pack::SourceKind kind_;
    bool resolved_ = false;
    engine::Pack::PackManifest manifest_;
    std::vector<engine::Pack::TreeNode> tree_;
    std::vector<engine::Pack::Rule> rules_;
    std::vector<engine::Pack::Patch> patches_;
    std::vector<engine::Pack::IniEdit> ini_edits_;
    std::vector<engine::Pack::ExecEntry> executables_;
    std::vector<engine::Pack::ChoiceGroup> choices_;
};

}  // namespace

TEST_CASE("pack detect_source_kind routes files and nxm urls", "[engine]") {
    using engine::Pack::SourceKind;
    REQUIRE(engine::Pack::detect_source_kind("/packs/foo.gmmpack") ==
            SourceKind::File);
    REQUIRE(engine::Pack::detect_source_kind("/packs/FOO.GMMpack") ==
            SourceKind::File);
    REQUIRE(engine::Pack::detect_source_kind("/packs/plain-dir") ==
            SourceKind::File);
    REQUIRE(engine::Pack::detect_source_kind(
                "nxm://skyrimspecialedition/collections/abc/revisions/1") ==
            SourceKind::NxmApi);
    REQUIRE(engine::Pack::detect_source_kind("NXM://skyrimspecialedition/x") ==
            SourceKind::NxmApi);
}

TEST_CASE("pack can_handle accepts gmmpack paths and nxm urls", "[engine]") {
    REQUIRE(engine::Pack::can_handle("/packs/foo.gmmpack"));
    REQUIRE(engine::Pack::can_handle("/packs/FOO.GMMPACK"));
    REQUIRE(engine::Pack::can_handle("nxm://skyrimspecialedition/mods/1/files/2"));
    REQUIRE_FALSE(engine::Pack::can_handle("/packs/readme.txt"));
    REQUIRE_FALSE(engine::Pack::can_handle("https://example.com/pack.zip"));
}

TEST_CASE("pack outcome carries success and failure", "[engine]") {
    auto ok =
        engine::Pack::Outcome<engine::Pack::PackManifest>::success({"Pack"});
    REQUIRE(ok.ok);
    REQUIRE(ok.value.name == "Pack");
    REQUIRE(ok.error.empty());
    auto err =
        engine::Pack::Outcome<engine::Pack::PackManifest>::failure("nope");
    REQUIRE_FALSE(err.ok);
    REQUIRE(err.error == "nope");
}

TEST_CASE("pack file adapter drives the full installer contract",
          "[engine]") {
    FakeAdapter adapter("gmmpack", engine::Pack::SourceKind::File);
    REQUIRE(adapter.format_id() == "gmmpack");

    // Before resolve_pack the content getters are empty.
    REQUIRE(adapter.get_tree().empty());
    REQUIRE(adapter.get_rules().empty());
    REQUIRE(adapter.get_patches().empty());
    REQUIRE(adapter.get_ini_edits().empty());
    REQUIRE(adapter.get_executables().empty());
    REQUIRE(adapter.get_choice_groups().empty());

    auto pack = adapter.resolve_pack("/packs/foo.gmmpack");
    REQUIRE(pack.ok);
    REQUIRE(pack.value.mods.size() == 2);
    REQUIRE(pack.value.game_id == "skyrimspecialedition");

    // Wrong source kind is a clean failure, not a crash.
    auto bad = adapter.resolve_pack("nxm://skyrimspecialedition/mods/1/files/2");
    REQUIRE_FALSE(bad.ok);
    REQUIRE_FALSE(bad.error.empty());

    auto mod = adapter.resolve_mod(pack.value.mods[0]);
    REQUIRE(mod.ok);
    REQUIRE(mod.value.entry_id == "aaa");
    REQUIRE(mod.value.source_type == "nexus");

    auto empty =
        adapter.resolve_mod(engine::Pack::ModEntry{});
    REQUIRE_FALSE(empty.ok);

    const auto info = adapter.get_download_info(mod.value);
    REQUIRE(info.archive_name == "aaa.zip");
    REQUIRE(info.display_name == "Mod A");

    const auto tree = adapter.get_tree();
    REQUIRE(tree.size() == 2);
    REQUIRE(tree[0].is_separator);
    REQUIRE(tree[1].parent_id == "sep");

    const auto rules = adapter.get_rules();
    REQUIRE(rules.size() == 1);
    REQUIRE(rules[0].type == engine::Pack::Rule::Type::After);

    REQUIRE(adapter.get_patches().size() == 1);
    REQUIRE(adapter.get_ini_edits().size() == 1);
    REQUIRE(adapter.get_executables().size() == 1);

    const auto choices = adapter.get_choice_groups();
    REQUIRE(choices.size() == 1);
    REQUIRE(choices[0].mode ==
            engine::Pack::ChoiceGroup::Mode::ExactlyOne);
    REQUIRE(choices[0].default_option == "2K");
}

TEST_CASE("pack nxm adapter serves api-based collections", "[engine]") {
    FakeAdapter adapter("nexus-collection", engine::Pack::SourceKind::NxmApi);
    REQUIRE(adapter.format_id() == "nexus-collection");
    REQUIRE(adapter.can_handle_source("nxm://skyrimspecialedition/mods/1/files/2"));
    REQUIRE_FALSE(adapter.can_handle_source("/packs/foo.gmmpack"));

    auto pack = adapter.resolve_pack(
        "nxm://skyrimspecialedition/collections/abc/revisions/1");
    REQUIRE(pack.ok);
    REQUIRE(pack.value.mods.size() == 2);
    REQUIRE(adapter.get_tree().size() == 2);
    REQUIRE(adapter.get_rules().size() == 1);
}

TEST_CASE("pack registry routes references to adapters", "[engine]") {
    engine::Pack::Registry reg;
    reg.register_adapter(
        std::make_unique<FakeAdapter>("gmmpack", engine::Pack::SourceKind::File));
    reg.register_adapter(std::make_unique<FakeAdapter>(
        "nexus-collection", engine::Pack::SourceKind::NxmApi));

    REQUIRE(reg.adapters().size() == 2);
    auto* file = reg.adapter_for("/packs/foo.gmmpack");
    REQUIRE(file != nullptr);
    REQUIRE(file->format_id() == "gmmpack");
    auto* api = reg.adapter_for("nxm://skyrimspecialedition/mods/1/files/2");
    REQUIRE(api != nullptr);
    REQUIRE(api->format_id() == "nexus-collection");
    // Plain paths fall back to the file adapter via detect_source_kind.
    REQUIRE(reg.adapter_for("/packs/plain-dir") == file);
}

TEST_CASE("pack value types default-construct sanely", "[engine]") {
    const engine::Pack::ModEntry e;
    REQUIRE(e.id.empty());
    REQUIRE_FALSE(e.optional);
    const engine::Pack::Rule r;
    REQUIRE(r.type == engine::Pack::Rule::Type::After);
    const engine::Pack::DownloadInfo d;
    REQUIRE(d.size_bytes == 0);
    const engine::Pack::ChoiceGroup c;
    REQUIRE(c.mode == engine::Pack::ChoiceGroup::Mode::ExactlyOne);
}
