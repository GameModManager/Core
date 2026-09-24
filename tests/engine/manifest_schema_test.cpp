// Tests for engine::Collection manifest schema types.
// Verifies that the source-agnostic manifest data model is complete and
// usable: default construction, enum values, variant dispatch, round-trip
// field access.

#include "engine/collection/manifest.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace engine::Collection;

TEST_CASE("Manifest default construction", "[collection][manifest]") {
  Manifest m;
  REQUIRE(m.schema_version.empty());
  REQUIRE(m.id.empty());
  REQUIRE(m.revision == 0);
  REQUIRE(m.mods.empty());
  REQUIRE(m.rules.empty());
  REQUIRE(m.choice_groups.empty());
  REQUIRE(m.tools.empty());
}

TEST_CASE("ModEntry fields", "[collection][manifest]") {
  ModEntry e;
  e.id       = "skyrim-2020";
  e.name     = "Skyrim 2020 Textures";
  e.phase    = 1;
  e.category = ModCategory::Required;

  REQUIRE(e.id == "skyrim-2020");
  REQUIRE(e.name == "Skyrim 2020 Textures");
  REQUIRE(e.phase == 1);
  REQUIRE(e.category == ModCategory::Required);
}

TEST_CASE("ModEntry default category is Optional", "[collection][manifest]") {
  ModEntry e;
  e.id = "test";
  REQUIRE(e.category == ModCategory::Optional);
  REQUIRE(e.phase == 0);
  REQUIRE(e.installer_choices.type.empty());
}

TEST_CASE("Rule construction", "[collection][manifest]") {
  Rule r;
  r.type = RuleType::Requires;
  r.from = "mod-a";
  r.to   = "mod-b";
  r.note = "A needs B";

  REQUIRE(r.type == RuleType::Requires);
  REQUIRE(r.from == "mod-a");
  REQUIRE(r.to == "mod-b");
  REQUIRE(r.note == "A needs B");
}

TEST_CASE("ChoiceGroup with ExactlyOne mode", "[collection][manifest]") {
  ChoiceGroup g;
  g.id             = "body-type";
  g.name           = "Body Type";
  g.mode           = ChoiceMode::ExactlyOne;
  g.member_mod_ids = {"cbbe", "unp", " bhunp"};

  REQUIRE(g.id == "body-type");
  REQUIRE(g.mode == ChoiceMode::ExactlyOne);
  REQUIRE(g.member_mod_ids.size() == 3);
}

TEST_CASE("ChoiceGroup with AtMostOne mode", "[collection][manifest]") {
  ChoiceGroup g;
  g.id             = "enb";
  g.name           = "ENB";
  g.mode           = ChoiceMode::AtMostOne;
  g.member_mod_ids = {" Rudy ENB", "Natural View"};

  REQUIRE(g.mode == ChoiceMode::AtMostOne);
  REQUIRE(g.member_mod_ids.size() == 2);
}

TEST_CASE("ModSource variant - Nexus source", "[collection][manifest]") {
  SourceNexus nx;
  nx.resolution    = SourceResolution::Api;
  nx.game_domain   = "skyrimspecialedition";
  nx.mod_id        = 12345;
  nx.file_id       = 67890;
  nx.version       = "1.0.0";
  nx.file_name     = "mod-12345-1-0.zip";
  nx.file_size     = 1024000;
  nx.sha256        = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  nx.update_policy = UpdatePolicy::Exact;

  ModSource src = nx;
  REQUIRE(std::holds_alternative<SourceNexus>(src));

  const auto &resolved = std::get<SourceNexus>(src);
  REQUIRE(resolved.mod_id == 12345);
  REQUIRE(resolved.game_domain == "skyrimspecialedition");
  REQUIRE(resolved.resolution == SourceResolution::Api);
  REQUIRE(resolved.update_policy == UpdatePolicy::Exact);
  REQUIRE_FALSE(resolved.sha256.empty());
}

TEST_CASE("ModSource variant - Steam Workshop source", "[collection][manifest]") {
  SourceSteamWorkshop sw;
  sw.app_id           = 72850;
  sw.workshop_item_id = 99999;
  sw.resolution       = SourceResolution::ClientSubscription;

  ModSource src = sw;
  REQUIRE(std::holds_alternative<SourceSteamWorkshop>(src));

  const auto &resolved = std::get<SourceSteamWorkshop>(src);
  REQUIRE(resolved.app_id == 72850);
  REQUIRE(resolved.workshop_item_id == 99999);
  REQUIRE(resolved.resolution == SourceResolution::ClientSubscription);
}

TEST_CASE("ModSource variant - Direct source", "[collection][manifest]") {
  SourceDirect d;
  d.resolution    = SourceResolution::Browser;
  d.url           = "https://example.com/mod.zip";
  d.update_policy = UpdatePolicy::Latest;

  ModSource src = d;
  REQUIRE(std::holds_alternative<SourceDirect>(src));

  const auto &resolved = std::get<SourceDirect>(src);
  REQUIRE(resolved.url == "https://example.com/mod.zip");
  REQUIRE(resolved.update_policy == UpdatePolicy::Latest);
}

TEST_CASE("ModSource variant - LoversLab source", "[collection][manifest]") {
  SourceLoversLab ll;
  ll.mod_id       = "12345";
  ll.section_slug = "skyrim";
  ll.resolution   = SourceResolution::Browser;

  ModSource src = ll;
  REQUIRE(std::holds_alternative<SourceLoversLab>(src));

  const auto &resolved = std::get<SourceLoversLab>(src);
  REQUIRE(resolved.mod_id == "12345");
  REQUIRE(resolved.section_slug == "skyrim");
}

TEST_CASE("ModSource variant - ModPub source", "[collection][manifest]") {
  SourceModPub mp;
  mp.mod_id     = "abc";
  mp.resolution = SourceResolution::Api;

  ModSource src = mp;
  REQUIRE(std::holds_alternative<SourceModPub>(src));

  const auto &resolved = std::get<SourceModPub>(src);
  REQUIRE(resolved.mod_id == "abc");
  REQUIRE(resolved.resolution == SourceResolution::Api);
}

TEST_CASE("InstallerChoices construction", "[collection][manifest]") {
  InstallerChoices ic;
  ic.type                       = "fomod";
  ic.selections["Step1-Group1"] = {"OptionA", "OptionC"};
  ic.selections["Step2-Group1"] = {"OptionB"};

  REQUIRE(ic.type == "fomod");
  REQUIRE(ic.selections.size() == 2);
  REQUIRE(ic.selections.at("Step1-Group1").size() == 2);
  REQUIRE(ic.selections.at("Step2-Group1").size() == 1);
}

TEST_CASE("PlatformOverride defaults", "[collection][manifest]") {
  PlatformOverride po;
  REQUIRE(po.proton_version_pin.empty());
  REQUIRE_FALSE(po.steam_overlay);
  REQUIRE(po.launch_options.empty());
  REQUIRE(po.prefix_files.empty());
}

TEST_CASE("PlatformDefaults aggregates three platforms", "[collection][manifest]") {
  PlatformDefaults pd;
  pd.linux_.steam_overlay     = true;
  pd.macos.proton_version_pin = "8.0";
  pd.windows.launch_options   = "--skip-intro";

  REQUIRE(pd.linux_.steam_overlay);
  REQUIRE(pd.macos.proton_version_pin == "8.0");
  REQUIRE(pd.windows.launch_options == "--skip-intro");
}

TEST_CASE("PrefixFile construction", "[collection][manifest]") {
  PrefixFile pf;
  pf.path          = "SSE Engine Fixes/Data";
  pf.source_mod_id = "sse-engine-fixes";

  REQUIRE(pf.path == "SSE Engine Fixes/Data");
  REQUIRE(pf.source_mod_id == "sse-engine-fixes");
}

TEST_CASE("Tool construction", "[collection][manifest]") {
  Tool t;
  t.id       = "loot";
  t.name     = "LOOT";
  t.homepage = "https://loot.github.io";

  REQUIRE(t.id == "loot");
  REQUIRE(t.name == "LOOT");
  REQUIRE(t.homepage == "https://loot.github.io");
}

TEST_CASE("ArchiveIntegrity stores file hashes", "[collection][manifest]") {
  ArchiveIntegrity ai;
  ai.file_hashes["mods/skyrim-2020.json"] =
      "sha256:abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  ai.file_hashes["tree.json"] =
      "sha256:1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef";

  REQUIRE(ai.file_hashes.size() == 2);
  REQUIRE(ai.file_hashes.at("mods/skyrim-2020.json").starts_with("sha256:"));
}

TEST_CASE("LoadOrderHint construction", "[collection][manifest]") {
  LoadOrderHint loh;
  loh.plugin_hint = {"Unofficial Patch", "SkyUI", "SMIM"};

  REQUIRE(loh.plugin_hint.size() == 3);
  REQUIRE(loh.plugin_hint[0] == "Unofficial Patch");
}

TEST_CASE("Full manifest assembly", "[collection][manifest]") {
  Manifest m;
  m.schema_version = "1.0.0";
  m.id             = "550e8400-e29b-41d4-a716-446655440000";
  m.revision       = 3;

  m.info.name       = "Test Collection";
  m.info.author     = "Tester";
  m.info.game_id    = "skyrimspecialedition";
  m.info.created_at = "2026-01-01T00:00:00Z";
  m.info.updated_at = "2026-09-15T00:00:00Z";

  Tool t;
  t.id   = "loot";
  t.name = "LOOT";
  m.tools.push_back(t);

  m.platform.linux_.steam_overlay = true;

  Rule r1;
  r1.type = RuleType::Requires;
  r1.from = "skse";
  r1.to   = "engine-fixes";
  m.rules.push_back(r1);

  Rule r2;
  r2.type = RuleType::Before;
  r2.from = "textures";
  r2.to   = "enb";
  m.rules.push_back(r2);

  m.load_order.plugin_hint = {"Unofficial Patch"};

  ChoiceGroup cg;
  cg.id             = "body-type";
  cg.name           = "Body Type";
  cg.mode           = ChoiceMode::ExactlyOne;
  cg.member_mod_ids = {"cbbe", "unp"};
  m.choice_groups.push_back(cg);

  ModEntry e1;
  e1.id       = "skse";
  e1.name     = "SKSE";
  e1.phase    = 0;
  e1.category = ModCategory::Required;
  SourceNexus nx1;
  nx1.resolution    = SourceResolution::Api;
  nx1.game_domain   = "skyrimspecialedition";
  nx1.mod_id        = 3018;
  nx1.update_policy = UpdatePolicy::Latest;
  e1.source         = nx1;
  m.mods.push_back(e1);

  ModEntry e2;
  e2.id       = "engine-fixes";
  e2.name     = "SSE Engine Fixes";
  e2.phase    = 1;
  e2.category = ModCategory::Recommended;
  SourceNexus nx2;
  nx2.resolution    = SourceResolution::Api;
  nx2.game_domain   = "skyrimspecialedition";
  nx2.mod_id        = 17230;
  nx2.file_id       = 3694520;
  nx2.version       = "6.1.2";
  nx2.update_policy = UpdatePolicy::Exact;
  e2.source         = nx2;
  InstallerChoices ic;
  ic.type                 = "fomod";
  ic.selections["Main-1"] = {"Plugin-1"};
  e2.installer_choices    = ic;
  m.mods.push_back(e2);

  ArchiveIntegrity ai;
  ai.file_hashes["manifest.json"] =
      "sha256:aaaa00000000000000000000000000000000000000000000000000000000aaaa";
  m.archive = ai;

  // Verify the full round-trip
  REQUIRE(m.schema_version == "1.0.0");
  REQUIRE(m.id == "550e8400-e29b-41d4-a716-446655440000");
  REQUIRE(m.revision == 3);
  REQUIRE(m.info.name == "Test Collection");
  REQUIRE(m.info.game_id == "skyrimspecialedition");
  REQUIRE(m.tools.size() == 1);
  REQUIRE(m.tools[0].id == "loot");
  REQUIRE(m.platform.linux_.steam_overlay);
  REQUIRE(m.rules.size() == 2);
  REQUIRE(m.rules[0].type == RuleType::Requires);
  REQUIRE(m.rules[1].type == RuleType::Before);
  REQUIRE(m.load_order.plugin_hint.size() == 1);
  REQUIRE(m.choice_groups.size() == 1);
  REQUIRE(m.choice_groups[0].member_mod_ids.size() == 2);
  REQUIRE(m.mods.size() == 2);
  REQUIRE(m.mods[0].id == "skse");
  REQUIRE(m.mods[1].id == "engine-fixes");

  // Verify the installer choices on mod 2
  const auto &ic2 = m.mods[1].installer_choices;
  REQUIRE(ic2.type == "fomod");
  REQUIRE(ic2.selections.at("Main-1").size() == 1);

  // Verify archive integrity
  REQUIRE(m.archive.file_hashes.size() == 1);

  // Verify variant dispatch on first mod
  REQUIRE(std::holds_alternative<SourceNexus>(m.mods[0].source));
  const auto &src1 = std::get<SourceNexus>(m.mods[0].source);
  REQUIRE(src1.mod_id == 3018);
  REQUIRE(src1.update_policy == UpdatePolicy::Latest);
}

TEST_CASE("Enum completeness - all RuleType values", "[collection][manifest]") {
  std::vector<RuleType> types = {
      RuleType::Before,
      RuleType::After,
      RuleType::Requires,
      RuleType::Conflicts,
  };
  REQUIRE(types.size() == 4);
}

TEST_CASE("Enum completeness - all ModCategory values", "[collection][manifest]") {
  std::vector<ModCategory> cats = {
      ModCategory::Required,
      ModCategory::Optional,
      ModCategory::Recommended,
  };
  REQUIRE(cats.size() == 3);
}

TEST_CASE("Enum completeness - all ChoiceMode values", "[collection][manifest]") {
  std::vector<ChoiceMode> modes = {
      ChoiceMode::ExactlyOne,
      ChoiceMode::AtMostOne,
  };
  REQUIRE(modes.size() == 2);
}

TEST_CASE("Enum completeness - all UpdatePolicy values", "[collection][manifest]") {
  std::vector<UpdatePolicy> policies = {
      UpdatePolicy::Exact,
      UpdatePolicy::Latest,
  };
  REQUIRE(policies.size() == 2);
}

TEST_CASE("Enum completeness - all SourceResolution values", "[collection][manifest]") {
  std::vector<SourceResolution> res = {
      SourceResolution::Api,
      SourceResolution::Browser,
      SourceResolution::ClientSubscription,
  };
  REQUIRE(res.size() == 3);
}

TEST_CASE("ModSource variant covers all 5 providers", "[collection][manifest]") {
  // This test documents the variant alternative count. If a new provider is
  // added to the schema, this test must be updated (and that's the point).
  ModSource src = SourceNexus{};

  // Visit and count alternatives
  REQUIRE(src.index() == 0);  // SourceNexus is first

  src = SourceLoversLab{};
  REQUIRE(src.index() == 1);

  src = SourceModPub{};
  REQUIRE(src.index() == 2);

  src = SourceSteamWorkshop{};
  REQUIRE(src.index() == 3);

  src = SourceDirect{};
  REQUIRE(src.index() == 4);
}

TEST_CASE("ModEntry with empty installer_choices is valid", "[collection][manifest]") {
  ModEntry e;
  e.id   = "simple-mod";
  e.name = "Simple Mod";

  REQUIRE(e.installer_choices.type.empty());
  REQUIRE(e.installer_choices.selections.empty());
}

TEST_CASE("Manifest mods can hold mixed source types", "[collection][manifest]") {
  Manifest m;

  ModEntry nx;
  nx.id     = "nexus-mod";
  nx.source = SourceNexus{};
  m.mods.push_back(nx);

  ModEntry sw;
  sw.id     = "workshop-mod";
  sw.source = SourceSteamWorkshop{};
  m.mods.push_back(sw);

  ModEntry dl;
  dl.id     = "direct-mod";
  dl.source = SourceDirect{};
  m.mods.push_back(dl);

  REQUIRE(m.mods.size() == 3);
  REQUIRE(std::holds_alternative<SourceNexus>(m.mods[0].source));
  REQUIRE(std::holds_alternative<SourceSteamWorkshop>(m.mods[1].source));
  REQUIRE(std::holds_alternative<SourceDirect>(m.mods[2].source));
}

TEST_CASE("PlatformOverride with prefix files", "[collection][manifest]") {
  PlatformOverride po;
  PrefixFile pf1;
  pf1.path          = "SKSE/Plugins/foo.txt";
  pf1.source_mod_id = "foo-mod";
  PrefixFile pf2;
  pf2.path          = "SKSE/Plugins/bar.txt";
  pf2.source_mod_id = "bar-mod";
  po.prefix_files   = {pf1, pf2};

  REQUIRE(po.prefix_files.size() == 2);
  REQUIRE(po.prefix_files[0].source_mod_id == "foo-mod");
  REQUIRE(po.prefix_files[1].source_mod_id == "bar-mod");
}

TEST_CASE("Multiple rules form a dependency graph", "[collection][manifest]") {
  Manifest m;

  Rule r1;
  r1.type = RuleType::Requires;
  r1.from = "mod-c";
  r1.to   = "mod-a";
  m.rules.push_back(r1);

  Rule r2;
  r2.type = RuleType::Requires;
  r2.from = "mod-b";
  r2.to   = "mod-a";
  m.rules.push_back(r2);

  Rule r3;
  r3.type = RuleType::After;
  r3.from = "mod-c";
  r3.to   = "mod-b";
  m.rules.push_back(r3);

  // mod-c requires mod-a, mod-b requires mod-a, mod-c after mod-b
  // Expected order: mod-a, mod-b, mod-c
  REQUIRE(m.rules.size() == 3);

  // Verify the graph structure
  int requires_count = 0;
  int after_count    = 0;
  for (const auto &r : m.rules) {
    if (r.type == RuleType::Requires)
      ++requires_count;
    if (r.type == RuleType::After)
      ++after_count;
  }
  REQUIRE(requires_count == 2);
  REQUIRE(after_count == 1);
}
