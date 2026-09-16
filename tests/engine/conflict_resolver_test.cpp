// Tests for the append-install conflict resolver: duplicate-source,
// duplicate-file and name-collision detection, safe-default resolution,
// and install-plan rewriting.
#include "engine/install/conflict_resolver.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace
{

using engine::Install::Action;
using engine::Install::Conflict;
using engine::Install::ConflictType;
using engine::Install::InstallPlan;
using engine::Install::PlanEntry;
using engine::Install::Resolution;
using engine::Install::UserChoice;
using engine::Pack::ResolvedMod;

ResolvedMod make_mod(std::string entry, std::string name, std::string archive,
                     std::string source_type = "nexus", std::string source_id = {},
                     std::string file_id = {})
{
  ResolvedMod m;
  m.entry_id     = std::move(entry);
  m.display_name = std::move(name);
  m.archive_name = std::move(archive);
  m.source_type  = std::move(source_type);
  m.source_id    = std::move(source_id);
  m.file_id      = std::move(file_id);
  return m;
}

const Conflict* find_by_pack(const std::vector<Conflict>& conflicts,
                             const std::string& pack_entry)
{
  for (const auto& c : conflicts) {
    if (c.pack.entry_id == pack_entry)
      return &c;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("conflict resolver reports no conflicts for disjoint mods", "[engine]")
{
  const std::vector<ResolvedMod> existing = {
      make_mod("e1", "Mod One", "one.zip", "nexus", "1")};
  const std::vector<ResolvedMod> pack = {
      make_mod("p1", "Mod Two", "two.zip", "nexus", "2")};
  REQUIRE(engine::Install::detect_conflicts(existing, pack).empty());
  REQUIRE(engine::Install::detect_conflicts({}, pack).empty());
  REQUIRE(engine::Install::detect_conflicts(existing, {}).empty());
}

TEST_CASE("conflict resolver detects duplicate source", "[engine]")
{
  const std::vector<ResolvedMod> existing = {
      make_mod("e1", "Mod One v1", "one-v1.zip", "nexus", "100", "1000")};
  // Same provider + mod id, newer file: still the same mod.
  const std::vector<ResolvedMod> pack = {
      make_mod("p1", "Mod One v2", "one-v2.zip", "nexus", "100", "2000")};
  const auto conflicts = engine::Install::detect_conflicts(existing, pack);
  REQUIRE(conflicts.size() == 1);
  REQUIRE(conflicts[0].type == ConflictType::DuplicateSource);
  REQUIRE(conflicts[0].existing.entry_id == "e1");
  REQUIRE(conflicts[0].pack.entry_id == "p1");
}

TEST_CASE("conflict resolver matches source case-insensitively", "[engine]")
{
  const std::vector<ResolvedMod> existing = {
      make_mod("e1", "Mod One", "one.zip", "Nexus", "100")};
  const std::vector<ResolvedMod> pack = {
      make_mod("p1", "Mod One", "one.zip", "nexus", "100")};
  const auto conflicts = engine::Install::detect_conflicts(existing, pack);
  REQUIRE(conflicts.size() == 1);
  REQUIRE(conflicts[0].type == ConflictType::DuplicateSource);
}

TEST_CASE("conflict resolver ignores unknown origins", "[engine]")
{
  // Both sides lack provider identity: must not false-positive, even with
  // identical names and archives... except the archive still collides.
  const std::vector<ResolvedMod> existing = {
      make_mod("e1", "Mod One", "one.zip", "", "")};
  const std::vector<ResolvedMod> pack = {
      make_mod("p1", "Other Name", "other.zip", "", "")};
  REQUIRE(engine::Install::detect_conflicts(existing, pack).empty());
}

TEST_CASE("conflict resolver detects duplicate file", "[engine]")
{
  const std::vector<ResolvedMod> existing = {
      make_mod("e1", "Alpha", "Shared.ZIP", "nexus", "1")};
  const std::vector<ResolvedMod> pack = {
      make_mod("p1", "Beta", "shared.zip", "loverslab", "2")};
  const auto conflicts = engine::Install::detect_conflicts(existing, pack);
  REQUIRE(conflicts.size() == 1);
  REQUIRE(conflicts[0].type == ConflictType::DuplicateFile);
}

TEST_CASE("conflict resolver ignores empty archive names", "[engine]")
{
  const std::vector<ResolvedMod> existing = {make_mod("e1", "Alpha", "", "nexus", "1")};
  const std::vector<ResolvedMod> pack = {make_mod("p1", "Beta", "", "loverslab", "2")};
  REQUIRE(engine::Install::detect_conflicts(existing, pack).empty());
}

TEST_CASE("conflict resolver detects name collisions", "[engine]")
{
  const std::vector<ResolvedMod> existing = {
      make_mod("e1", "Cool Mod", "cool-a.zip", "nexus", "1")};
  const std::vector<ResolvedMod> pack = {
      make_mod("p1", "cool mod", "cool-b.zip", "direct", "https://x/y.zip")};
  const auto conflicts = engine::Install::detect_conflicts(existing, pack);
  REQUIRE(conflicts.size() == 1);
  REQUIRE(conflicts[0].type == ConflictType::NameCollision);
}

TEST_CASE("conflict resolver reports one conflict per pair, strongest wins", "[engine]")
{
  const std::vector<ResolvedMod> existing = {
      make_mod("e1", "Same", "same.zip", "nexus", "1")};
  // Same source AND same file AND same name: exactly one DuplicateSource.
  const std::vector<ResolvedMod> pack = {
      make_mod("p1", "Same", "same.zip", "nexus", "1")};
  const auto both = engine::Install::detect_conflicts(existing, pack);
  REQUIRE(both.size() == 1);
  REQUIRE(both[0].type == ConflictType::DuplicateSource);

  // Same file AND same name, different source: exactly one DuplicateFile.
  const std::vector<ResolvedMod> pack2 = {
      make_mod("p2", "Same", "same.zip", "loverslab", "9")};
  const auto file_name = engine::Install::detect_conflicts(existing, pack2);
  REQUIRE(file_name.size() == 1);
  REQUIRE(file_name[0].type == ConflictType::DuplicateFile);
}

TEST_CASE("conflict resolver detects several pack mods at once", "[engine]")
{
  const std::vector<ResolvedMod> existing = {
      make_mod("e1", "Mod One", "one.zip", "nexus", "1"),
      make_mod("e2", "Mod Two", "two.zip", "nexus", "2")};
  const std::vector<ResolvedMod> pack = {
      make_mod("p1", "Mod One v2", "one-v2.zip", "nexus", "1"),
      make_mod("p2", "Mod Two", "two.zip", "loverslab", "7"),
      make_mod("p3", "Fresh", "fresh.zip", "nexus", "3")};
  const auto conflicts = engine::Install::detect_conflicts(existing, pack);
  REQUIRE(conflicts.size() == 2);
  const auto* c1 = find_by_pack(conflicts, "p1");
  const auto* c2 = find_by_pack(conflicts, "p2");
  REQUIRE(c1 != nullptr);
  REQUIRE(c1->type == ConflictType::DuplicateSource);
  REQUIRE(c2 != nullptr);
  REQUIRE(c2->type == ConflictType::DuplicateFile);
  REQUIRE(find_by_pack(conflicts, "p3") == nullptr);
}

TEST_CASE("conflict resolver defaults are safe without user choices", "[engine]")
{
  const std::vector<Conflict> conflicts = {
      {ConflictType::DuplicateSource, make_mod("e1", "A", "a.zip"),
       make_mod("p1", "A", "a2.zip")},
      {ConflictType::DuplicateFile, make_mod("e2", "B", "b.zip"),
       make_mod("p2", "C", "b.zip")},
      {ConflictType::NameCollision, make_mod("e3", "D", "d.zip"),
       make_mod("p3", "D", "e.zip")},
  };
  const auto resolutions = engine::Install::resolve_conflicts(conflicts, {});
  REQUIRE(resolutions.size() == 3);
  REQUIRE(resolutions[0].action == Action::Skip);
  REQUIRE(resolutions[1].action == Action::Skip);
  REQUIRE(resolutions[2].action == Action::Keep);
  for (std::size_t i = 0; i < resolutions.size(); ++i) {
    REQUIRE(resolutions[i].conflict_index == i);
  }
  REQUIRE(resolutions[0].pack_entry_id == "p1");
  // Ask defers to the same defaults.
  const auto asked = engine::Install::resolve_conflicts(
      conflicts, std::vector<UserChoice>{{0, Action::Ask, ""}});
  REQUIRE(asked[0].action == Action::Skip);
}

TEST_CASE("conflict resolver honors explicit user choices", "[engine]")
{
  const std::vector<Conflict> conflicts = {
      {ConflictType::DuplicateSource, make_mod("e1", "A", "a.zip"),
       make_mod("p1", "A", "a2.zip")},
      {ConflictType::DuplicateFile, make_mod("e2", "B", "b.zip"),
       make_mod("p2", "C", "b.zip")},
  };
  const std::vector<UserChoice> choices = {{0, Action::Replace, ""},
                                           {1, Action::Rename, "c-renamed.zip"}};
  const auto resolutions = engine::Install::resolve_conflicts(conflicts, choices);
  REQUIRE(resolutions.size() == 2);
  REQUIRE(resolutions[0].action == Action::Replace);
  REQUIRE(resolutions[0].remove_existing == "e1");
  REQUIRE(resolutions[1].action == Action::Rename);
  REQUIRE(resolutions[1].rename_to == "c-renamed.zip");
  REQUIRE(resolutions[1].remove_existing.empty());
}

TEST_CASE("conflict resolver handles bad choices safely", "[engine]")
{
  const std::vector<Conflict> conflicts = {
      {ConflictType::DuplicateFile, make_mod("e1", "A", "a.zip"),
       make_mod("p1", "B", "a.zip")},
  };
  // Out-of-range index is ignored; empty rename falls back to Skip;
  // last choice for one conflict wins.
  const std::vector<UserChoice> choices = {
      {7, Action::Replace, ""},
      {0, Action::Rename, ""},
      {0, Action::Rename, "b.zip"},
      {0, Action::Skip, ""},
  };
  const auto resolutions = engine::Install::resolve_conflicts(conflicts, choices);
  REQUIRE(resolutions.size() == 1);
  REQUIRE(resolutions[0].action == Action::Skip);
  REQUIRE(resolutions[0].rename_to.empty());

  const auto renamed = engine::Install::resolve_conflicts(
      conflicts, std::vector<UserChoice>{{0, Action::Rename, "b.zip"}});
  REQUIRE(renamed[0].action == Action::Rename);
}

TEST_CASE("conflict resolver rewrites the install plan", "[engine]")
{
  const InstallPlan plan = {
      {make_mod("p1", "A", "a.zip"), ""},
      {make_mod("p2", "B", "b.zip"), ""},
      {make_mod("p3", "C", "c.zip"), ""},
  };
  const std::vector<Resolution> resolutions = {
      {0, Action::Skip, "p1", "", ""},
      {1, Action::Replace, "p2", "", "e2"},
      {2, Action::Rename, "p3", "c-renamed.zip", ""},
  };
  const InstallPlan out = engine::Install::apply_resolutions(resolutions, plan);
  REQUIRE(out.size() == 2);
  REQUIRE(out[0].mod.entry_id == "p2");
  REQUIRE(out[0].target_name.empty());
  REQUIRE(out[1].mod.entry_id == "p3");
  REQUIRE(out[1].target_name == "c-renamed.zip");
}

TEST_CASE("conflict resolver leaves untouched entries alone", "[engine]")
{
  const InstallPlan plan = {
      {make_mod("p1", "A", "a.zip"), ""},
      {make_mod("p9", "Z", "z.zip"), "custom.zip"},
  };
  // Unknown entry ids are ignored; input plan is not modified.
  const std::vector<Resolution> resolutions = {
      {0, Action::Skip, "nope", "", ""},
  };
  const InstallPlan out = engine::Install::apply_resolutions(resolutions, plan);
  REQUIRE(out.size() == 2);
  REQUIRE(out[1].target_name == "custom.zip");
  REQUIRE(engine::Install::apply_resolutions({}, {}).empty());
}

TEST_CASE("conflict resolver strongest action wins per entry", "[engine]")
{
  const InstallPlan plan                    = {{make_mod("p1", "A", "a.zip"), ""}};
  const std::vector<Resolution> resolutions = {
      {0, Action::Rename, "p1", "a2.zip", ""},
      {1, Action::Skip, "p1", "", ""},
  };
  REQUIRE(engine::Install::apply_resolutions(resolutions, plan).empty());

  const std::vector<Resolution> replace_vs_rename = {
      {0, Action::Rename, "p1", "a2.zip", ""},
      {1, Action::Replace, "p1", "", "e1"},
  };
  const InstallPlan kept = engine::Install::apply_resolutions(replace_vs_rename, plan);
  REQUIRE(kept.size() == 1);
  REQUIRE(kept[0].target_name.empty());
}

TEST_CASE("conflict resolver describes conflicts and resolutions", "[engine]")
{
  const Conflict c{ConflictType::NameCollision, make_mod("e1", "A", "a.zip"),
                   make_mod("p1", "A", "b.zip")};
  const std::string text = engine::Install::describe(c);
  REQUIRE(text.find("name-collision") != std::string::npos);
  REQUIRE(text.find("p1") != std::string::npos);
  REQUIRE(text.find("e1") != std::string::npos);

  const Conflict src{ConflictType::DuplicateSource, make_mod("e1", "A", "a.zip"),
                     make_mod("p1", "A", "a2.zip")};
  REQUIRE(engine::Install::describe(src).find("duplicate-source") != std::string::npos);
  const Conflict file{ConflictType::DuplicateFile, make_mod("e1", "A", "a.zip"),
                      make_mod("p1", "B", "a.zip")};
  REQUIRE(engine::Install::describe(file).find("duplicate-file") != std::string::npos);

  const Resolution r{0, Action::Replace, "p1", "", "e1"};
  const std::string rtext = engine::Install::describe(r);
  REQUIRE(rtext.find("replace") != std::string::npos);
  REQUIRE(rtext.find("e1") != std::string::npos);
  const Resolution rn{1, Action::Rename, "p2", "p2b.zip", ""};
  REQUIRE(engine::Install::describe(rn).find("p2b.zip") != std::string::npos);
}
