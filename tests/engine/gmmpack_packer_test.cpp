// Tests for the .gmmpack export engine (packer.h).
//
// Covers:
//   - resolve_mod_source for nexus / loverslab / modpub / steam / direct,
//     manual skipped
//   - mod_slug mapping
//   - build_tree flat and nested (separators, ordering, enabled flags,
//     manual mods omitted)
//   - build_manifest fields (schema, revision, info, timestamps)
//   - build_mod_entries skips manual, deterministic order
//   - update_policies override (latest default, exact propagates and
//     round-trips, steam stays latest)
//   - build_executables mapping + unresolvable skipped
//   - build_gmmpack round-trip (serialize -> parse_manifest/parse_mod_entry/
//     parse_tree back)
//   - create_gmmpack writes an archive that passes extract + integrity check
//   - generated manifest/mod JSON validates against the input/ schemas
//   - colliding mods ship no patches/, both stay in the pack

#include "engine/gmmpack/packer.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "engine/gmmpack/schema_validator.h"
#include "engine/gmmpack/tree_parser.h"
#include "engine/gmmpack/unpacker.h"
#include "engine/mod/meta/mod_meta.h"

namespace fs      = std::filesystem;
namespace gmmpack = engine::gmmpack;

// ---------------------------------------------------------------------------
// Fixtures: temp mods dir with in-folder meta.ini files
// ---------------------------------------------------------------------------

struct TempDir {
  fs::path root;
  TempDir() {
    static int counter = 0;
    root               = fs::temp_directory_path() /
                         ("gmmpack_packer_test_" + std::to_string(::getpid()) + "_" +
                          std::to_string(counter++));
    fs::create_directories(root);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

static void write_meta(const fs::path &mods_dir, const std::string &folder,
                       engine::ModMeta meta) {
  meta.set("GameModManager", "folder", folder);
  REQUIRE(meta.save(mods_dir, folder));
}

static void nexus_mod(const fs::path &mods_dir, const std::string &folder,
                      const std::string &mod_id = "12345",
                      const std::string &fileid = "67890") {
  auto meta =
      engine::ModMeta::from_default(folder, "nexus", mod_id, "SkyUI-1.4.2.7z", "1.4.2");
  meta.set("Nexusmods", "modid", mod_id);
  meta.set("Nexusmods", "mod_id", mod_id);
  meta.set("Nexusmods", "fileid", fileid);
  write_meta(mods_dir, folder, meta);
}

static void manual_mod(const fs::path &mods_dir, const std::string &folder) {
  write_meta(mods_dir, folder, engine::ModMeta::from_default(folder, "manual", ""));
}

static engine::InstanceSnapshot make_snapshot() {
  engine::InstanceSnapshot snap;
  snap.game_id      = "skyrimspecialedition";
  snap.display_name = "Test Pack";
  snap.steam_appid  = 489830;
  return snap;
}

static void track(engine::InstanceSnapshot &snap, const std::string &folder,
                  int32_t pos, const std::string &parent = "", bool collapsed = false,
                  bool hidden = false, bool disabled = false) {
  engine::ModTrackingEntry e;
  e.list_position          = pos;
  e.parent_separator       = parent;
  e.collapsed              = collapsed;
  e.hidden                 = hidden;
  e.disabled               = disabled;
  snap.mod_entries[folder] = e;
}

// ---------------------------------------------------------------------------
// resolve_mod_source
// ---------------------------------------------------------------------------

TEST_CASE("packer resolve: nexus", "[gmmpack][packer]") {
  TempDir td;
  nexus_mod(td.root, "SkyUI");
  auto meta = engine::ModMeta::load(td.root, "SkyUI");

  auto src = gmmpack::resolve_mod_source(meta, "skyrimspecialedition", 489830);
  REQUIRE(src.has_value());
  auto *n = std::get_if<gmmpack::ModSourceNexus>(&*src);
  REQUIRE(n != nullptr);
  REQUIRE(n->mod_id == 12345);
  REQUIRE(n->file_id.has_value());
  REQUIRE(*n->file_id == 67890);
  REQUIRE(n->version.has_value());
  REQUIRE(*n->version == "1.4.2");
  REQUIRE(n->file_name.has_value());
  REQUIRE(n->update_policy == "latest");
  // gameDomain falls back to the GMM game id (meta carries no domain).
  REQUIRE(n->game_domain == "skyrimspecialedition");
}

TEST_CASE("packer resolve: nexus without fileid is browser resolution",
          "[gmmpack][packer]") {
  auto meta = engine::ModMeta::from_default("M", "nexus", "999", "", "2.0");
  auto src  = gmmpack::resolve_mod_source(meta, "skyrim", 0);
  REQUIRE(src.has_value());
  auto *n = std::get_if<gmmpack::ModSourceNexus>(&*src);
  REQUIRE(n != nullptr);
  REQUIRE(n->resolution == "browser");
  REQUIRE_FALSE(n->file_id.has_value());
}

TEST_CASE("packer resolve: steam", "[gmmpack][packer]") {
  auto meta = engine::ModMeta::from_default("M", "steam", "12345678", "", "1.0");
  auto src  = gmmpack::resolve_mod_source(meta, "skyrim", 489830);
  REQUIRE(src.has_value());
  auto *s = std::get_if<gmmpack::ModSourceSteamWorkshop>(&*src);
  REQUIRE(s != nullptr);
  REQUIRE(s->workshop_item_id == 12345678);
  REQUIRE(s->app_id == 489830);
  REQUIRE(s->update_policy == "latest");
}

TEST_CASE("packer resolve: steam_workshop alias", "[gmmpack][packer]") {
  auto meta = engine::ModMeta::from_default("M", "steam_workshop", "42");
  auto src  = gmmpack::resolve_mod_source(meta, "skyrim", 72850);
  REQUIRE(src.has_value());
  auto *s = std::get_if<gmmpack::ModSourceSteamWorkshop>(&*src);
  REQUIRE(s != nullptr);
  REQUIRE(s->workshop_item_id == 42);
  REQUIRE(s->app_id == 72850);
}

TEST_CASE("packer resolve: loverslab", "[gmmpack][packer]") {
  auto meta = engine::ModMeta::from_default("M", "loverslab", "777", "", "3.0");
  meta.set("LoversLab", "fileid", "777");
  meta.set("LoversLab", "page_url", "https://www.loverslab.com/files/file/777/");
  auto src = gmmpack::resolve_mod_source(meta, "skyrim", 0);
  REQUIRE(src.has_value());
  auto *l = std::get_if<gmmpack::ModSourceLoversLab>(&*src);
  REQUIRE(l != nullptr);
  REQUIRE(std::get<int64_t>(l->mod_id) == 777);
  REQUIRE(l->resolution == "browser");
}

TEST_CASE("packer resolve: modpub", "[gmmpack][packer]") {
  auto meta = engine::ModMeta::from_default("M", "modpub", "99", "", "1.1");
  meta.set("ModPub", "mod_id", "99");
  auto src = gmmpack::resolve_mod_source(meta, "skyrim", 0);
  REQUIRE(src.has_value());
  auto *p = std::get_if<gmmpack::ModSourceModPub>(&*src);
  REQUIRE(p != nullptr);
  REQUIRE(std::get<int64_t>(p->mod_id) == 99);
}

TEST_CASE("packer resolve: direct", "[gmmpack][packer]") {
  auto meta = engine::ModMeta::from_default(
      "M", "direct", "https://example.com/mods/a.zip", "", "1.0");
  auto src = gmmpack::resolve_mod_source(meta, "skyrim", 0);
  REQUIRE(src.has_value());
  auto *d = std::get_if<gmmpack::ModSourceDirect>(&*src);
  REQUIRE(d != nullptr);
  REQUIRE(d->url == "https://example.com/mods/a.zip");
}

TEST_CASE("packer resolve: manual is skipped", "[gmmpack][packer]") {
  auto meta = engine::ModMeta::from_default("M", "manual", "");
  REQUIRE_FALSE(gmmpack::resolve_mod_source(meta, "skyrim", 0).has_value());
}

TEST_CASE("packer resolve: unknown type is skipped", "[gmmpack][packer]") {
  auto meta = engine::ModMeta::from_default("M", "bogus", "x");
  REQUIRE_FALSE(gmmpack::resolve_mod_source(meta, "skyrim", 0).has_value());
}

// ---------------------------------------------------------------------------
// mod_slug
// ---------------------------------------------------------------------------

TEST_CASE("packer slug: mapping", "[gmmpack][packer]") {
  REQUIRE(gmmpack::mod_slug("SkyUI") == "skyui");
  REQUIRE(gmmpack::mod_slug("My Cool Mod!") == "my-cool-mod");
  REQUIRE(gmmpack::mod_slug("a__b--c") == "a-b-c");
  REQUIRE(gmmpack::mod_slug("") == "mod");
  REQUIRE(gmmpack::mod_slug("---") == "mod");
}

// ---------------------------------------------------------------------------
// build_tree
// ---------------------------------------------------------------------------

TEST_CASE("packer tree: flat list sorted by position", "[gmmpack][packer]") {
  TempDir td;
  nexus_mod(td.root, "ModB");
  nexus_mod(td.root, "ModA");
  auto snap = make_snapshot();
  track(snap, "ModB", 0);
  track(snap, "ModA", 1);

  auto tree = gmmpack::build_tree(snap, td.root);
  REQUIRE(tree.nodes.size() == 2);
  auto *first  = std::get_if<gmmpack::ModNode>(&tree.nodes[0].data);
  auto *second = std::get_if<gmmpack::ModNode>(&tree.nodes[1].data);
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  REQUIRE(first->id == "modb");
  REQUIRE(second->id == "moda");
  REQUIRE(first->enabled);
}

TEST_CASE("packer tree: nested separators", "[gmmpack][packer]") {
  TempDir td;
  nexus_mod(td.root, "SkyUI");
  nexus_mod(td.root, "ENB");
  auto snap = make_snapshot();
  // Separator entry (referenced as a parent).
  track(snap, "Graphics", 0, "", true);
  track(snap, "SkyUI", 1);
  track(snap, "ENB", 0, "Graphics");

  auto tree = gmmpack::build_tree(snap, td.root);
  REQUIRE(tree.nodes.size() == 2);
  auto *sep = std::get_if<gmmpack::SeparatorNode>(&tree.nodes[0].data);
  REQUIRE(sep != nullptr);
  REQUIRE(sep->name == "Graphics");
  REQUIRE(sep->collapsed);
  REQUIRE(sep->children.size() == 1);
  auto *child = std::get_if<gmmpack::ModNode>(&sep->children[0].data);
  REQUIRE(child != nullptr);
  REQUIRE(child->id == "enb");
  auto *top = std::get_if<gmmpack::ModNode>(&tree.nodes[1].data);
  REQUIRE(top != nullptr);
  REQUIRE(top->id == "skyui");
}

TEST_CASE("packer tree: hidden/disabled mods marked not enabled", "[gmmpack][packer]") {
  TempDir td;
  nexus_mod(td.root, "A");
  nexus_mod(td.root, "B");
  auto snap = make_snapshot();
  track(snap, "A", 0, "", false, true /*hidden*/);
  track(snap, "B", 1, "", false, false, true /*disabled*/);

  auto tree = gmmpack::build_tree(snap, td.root);
  REQUIRE(tree.nodes.size() == 2);
  REQUIRE_FALSE(std::get<gmmpack::ModNode>(tree.nodes[0].data).enabled);
  REQUIRE_FALSE(std::get<gmmpack::ModNode>(tree.nodes[1].data).enabled);
}

TEST_CASE("packer tree: manual mods omitted", "[gmmpack][packer]") {
  TempDir td;
  nexus_mod(td.root, "Good");
  manual_mod(td.root, "Local");
  auto snap = make_snapshot();
  track(snap, "Good", 0);
  track(snap, "Local", 1);

  auto tree = gmmpack::build_tree(snap, td.root);
  REQUIRE(tree.nodes.size() == 1);
  REQUIRE(std::get<gmmpack::ModNode>(tree.nodes[0].data).id == "good");
}

TEST_CASE("packer tree: separator color round-trips", "[gmmpack][packer]") {
  TempDir td;
  nexus_mod(td.root, "ENB");
  nexus_mod(td.root, "Tool");
  auto snap = make_snapshot();
  track(snap, "Graphics", 0);
  snap.mod_entries["Graphics"].separator_color = "#ff0000";
  track(snap, "ENB", 0, "Graphics");
  track(snap, "Plain", 1);
  track(snap, "Tool", 0, "Plain");

  auto tree = gmmpack::build_tree(snap, td.root);
  REQUIRE(tree.nodes.size() == 2);
  auto *sep = std::get_if<gmmpack::SeparatorNode>(&tree.nodes[0].data);
  REQUIRE(sep != nullptr);
  REQUIRE(sep->color == "#ff0000");
  auto *plain = std::get_if<gmmpack::SeparatorNode>(&tree.nodes[1].data);
  REQUIRE(plain != nullptr);
  REQUIRE(plain->color.empty());

  auto j = gmmpack::serialize_tree(tree);
  REQUIRE(j["nodes"][0]["color"] == "#ff0000");
  // Colorless separators omit the key (keeps schema validation green).
  REQUIRE_FALSE(j["nodes"][1].contains("color"));

  auto back  = gmmpack::parse_tree(j);
  auto *bsep = std::get_if<gmmpack::SeparatorNode>(&back.nodes[0].data);
  REQUIRE(bsep != nullptr);
  REQUIRE(bsep->color == "#ff0000");
  auto *bplain = std::get_if<gmmpack::SeparatorNode>(&back.nodes[1].data);
  REQUIRE(bplain != nullptr);
  REQUIRE(bplain->color.empty());
}

// ---------------------------------------------------------------------------
// build_manifest
// ---------------------------------------------------------------------------

TEST_CASE("packer manifest: fields", "[gmmpack][packer]") {
  auto snap = make_snapshot();
  gmmpack::PackOptions opts;
  opts.author      = "Author";
  opts.description = "Desc";
  opts.homepage    = "https://example.com";

  auto m = gmmpack::build_manifest(snap, opts);
  REQUIRE(m.gmmpack_schema == "1.0.0");
  REQUIRE(m.revision == 1);
  REQUIRE(m.id.size() == 36);
  REQUIRE(m.id[8] == '-');
  REQUIRE(m.id[14] == '4');  // UUID v4 version nibble
  REQUIRE(m.info.name == "Test Pack");
  REQUIRE(m.info.author == "Author");
  REQUIRE(m.info.description == "Desc");
  REQUIRE(m.info.gmm_game_id == "skyrimspecialedition");
  REQUIRE(m.info.homepage == "https://example.com");
  REQUIRE(!m.info.created_at.empty());
  REQUIRE(m.info.created_at == m.info.updated_at);
  REQUIRE(m.info.created_at.back() == 'Z');
}

TEST_CASE("packer manifest: reuses instance modpack_id and bumps revision",
          "[gmmpack][packer]") {
  auto snap             = make_snapshot();
  snap.modpack_id       = "11111111-2222-4333-8444-555555555555";
  snap.modpack_revision = 4;
  gmmpack::PackOptions opts;

  auto m = gmmpack::build_manifest(snap, opts);
  REQUIRE(m.id == "11111111-2222-4333-8444-555555555555");
  REQUIRE(m.revision == 5);
}

TEST_CASE("packer manifest: fallbacks for empty fields", "[gmmpack][packer]") {
  engine::InstanceSnapshot snap;
  gmmpack::PackOptions opts;
  auto m = gmmpack::build_manifest(snap, opts);
  REQUIRE(!m.info.name.empty());
  REQUIRE(!m.info.author.empty());
  REQUIRE(!m.info.gmm_game_id.empty());
  // Optional fields stay empty so serializers omit them.
  REQUIRE(m.info.description.empty());
  REQUIRE(m.info.homepage.empty());
}

TEST_CASE("packer manifest: picks up default-profile settings", "[gmmpack][packer]") {
  auto snap            = make_snapshot();
  snap.deploy_strategy = "symlink";
  engine::ProfileSnapshot prof;
  prof.name                      = "Default";
  prof.local_saves               = true;
  prof.local_settings            = false;
  prof.auto_archive_invalidation = true;
  snap.profiles.push_back(prof);

  gmmpack::PackOptions opts;
  auto m = gmmpack::build_manifest(snap, opts);
  REQUIRE(m.instance_settings.local_saves);
  REQUIRE_FALSE(m.instance_settings.local_settings);
  REQUIRE(m.instance_settings.auto_archive_invalidation);
  REQUIRE(m.instance_settings.deploy_strategy == "symlink");
}

TEST_CASE("packer manifest: empty profiles gives default settings",
          "[gmmpack][packer]") {
  auto snap            = make_snapshot();
  snap.deploy_strategy = "hardlink";
  gmmpack::PackOptions opts;
  auto m = gmmpack::build_manifest(snap, opts);
  REQUIRE_FALSE(m.instance_settings.local_saves);
  REQUIRE_FALSE(m.instance_settings.local_settings);
  REQUIRE_FALSE(m.instance_settings.auto_archive_invalidation);
  // No profiles: only the snapshot-level strategy survives.
  REQUIRE(m.instance_settings.deploy_strategy == "hardlink");
}

TEST_CASE("packer manifest: instance settings round-trip", "[gmmpack][packer]") {
  gmmpack::Manifest m;
  m.info.name                                   = "P";
  m.info.author                                 = "A";
  m.info.gmm_game_id                            = "skyrim";
  m.instance_settings.local_saves               = true;
  m.instance_settings.local_settings            = true;
  m.instance_settings.auto_archive_invalidation = true;
  m.instance_settings.deploy_strategy           = "symlink";

  auto j = gmmpack::serialize_manifest(m);
  REQUIRE(j.contains("instanceSettings"));
  REQUIRE(j["instanceSettings"]["localSaves"] == true);
  REQUIRE(j["instanceSettings"]["localSettings"] == true);
  REQUIRE(j["instanceSettings"]["automaticArchiveInvalidation"] == true);
  REQUIRE(j["instanceSettings"]["deployStrategy"] == "symlink");

  auto back = gmmpack::parse_manifest(j);
  REQUIRE(back.instance_settings.local_saves);
  REQUIRE(back.instance_settings.local_settings);
  REQUIRE(back.instance_settings.auto_archive_invalidation);
  REQUIRE(back.instance_settings.deploy_strategy == "symlink");
}

TEST_CASE("packer manifest: settings unset omits strategy, parses to defaults",
          "[gmmpack][packer]") {
  gmmpack::Manifest m;
  auto j = gmmpack::serialize_manifest(m);
  REQUIRE(j.contains("instanceSettings"));
  REQUIRE_FALSE(j["instanceSettings"].contains("deployStrategy"));

  auto back = gmmpack::parse_manifest(j);
  REQUIRE_FALSE(back.instance_settings.local_saves);
  REQUIRE_FALSE(back.instance_settings.local_settings);
  REQUIRE_FALSE(back.instance_settings.auto_archive_invalidation);
  REQUIRE(back.instance_settings.deploy_strategy.empty());

  // Backward compat: packs exported before instanceSettings existed.
  j.erase("instanceSettings");
  auto legacy = gmmpack::parse_manifest(j);
  REQUIRE_FALSE(legacy.instance_settings.local_saves);
  REQUIRE_FALSE(legacy.instance_settings.local_settings);
  REQUIRE_FALSE(legacy.instance_settings.auto_archive_invalidation);
  REQUIRE(legacy.instance_settings.deploy_strategy.empty());
}

// ---------------------------------------------------------------------------
// build_mod_entries
// ---------------------------------------------------------------------------

TEST_CASE("packer mod entries: skips manual, deterministic order",
          "[gmmpack][packer]") {
  TempDir td;
  nexus_mod(td.root, "Zeta");
  manual_mod(td.root, "Local");
  nexus_mod(td.root, "Alpha");
  auto snap = make_snapshot();
  track(snap, "Zeta", 1);
  track(snap, "Local", 2);
  track(snap, "Alpha", 0);

  gmmpack::PackOptions opts;
  auto mods = gmmpack::build_mod_entries(snap, td.root, opts);
  REQUIRE(mods.size() == 2);
  REQUIRE(mods[0].id == "alpha");
  REQUIRE(mods[1].id == "zeta");
  REQUIRE(mods[0].name == "Alpha");
  // A mod nobody labelled is required: every mod in an exported pack was put
  // there deliberately, and phase follows (see phase_for in packer.cpp).
  REQUIRE(mods[0].category == gmmpack::ModCategory::Required);
  REQUIRE(mods[0].phase == 0);

  // Bundling the manual mod does not change that - embed_folders only decides
  // where the bytes come from, never what the importer has to install.
  opts.embed_folders.insert("Local");
  auto embedded = gmmpack::build_mod_entries(snap, td.root, opts);
  REQUIRE(embedded.size() == 3);
  REQUIRE(embedded[2].id == "local");
  REQUIRE(embedded[2].category == gmmpack::ModCategory::Required);
  REQUIRE(embedded[2].phase == 0);
}

TEST_CASE("packer categories: category drives phase and ships in the pack",
          "[gmmpack][packer]") {
  TempDir td;
  nexus_mod(td.root, "SkyUI");
  auto snap = make_snapshot();
  track(snap, "SkyUI", 0);

  gmmpack::PackOptions opts;
  opts.categories["SkyUI"] = "optional";
  auto mods                = gmmpack::build_mod_entries(snap, td.root, opts);
  REQUIRE(mods.size() == 1);
  REQUIRE(mods[0].category == gmmpack::ModCategory::Optional);
  REQUIRE(mods[0].phase == 2);

  const auto json = gmmpack::serialize_mod_entry(mods[0]);
  REQUIRE(json["category"] == "optional");
  REQUIRE(json["phase"] == 2);
}

TEST_CASE("packer policies: default is latest", "[gmmpack][packer]") {
  TempDir td;
  nexus_mod(td.root, "SkyUI");
  auto snap = make_snapshot();
  track(snap, "SkyUI", 0);

  gmmpack::PackOptions opts;
  auto mods = gmmpack::build_mod_entries(snap, td.root, opts);
  REQUIRE(mods.size() == 1);
  auto *n = std::get_if<gmmpack::ModSourceNexus>(&mods[0].source);
  REQUIRE(n != nullptr);
  REQUIRE(n->update_policy == "latest");
}

TEST_CASE("packer policies: steam stays latest when exact requested",
          "[gmmpack][packer]") {
  TempDir td;
  auto meta = engine::ModMeta::from_default("SteamMod", "steam", "12345678");
  write_meta(td.root, "SteamMod", meta);
  auto snap = make_snapshot();
  track(snap, "SteamMod", 0);

  gmmpack::PackOptions opts;
  opts.update_policies["SteamMod"] = "exact";
  auto mods                        = gmmpack::build_mod_entries(snap, td.root, opts);
  REQUIRE(mods.size() == 1);
  auto *s = std::get_if<gmmpack::ModSourceSteamWorkshop>(&mods[0].source);
  REQUIRE(s != nullptr);
  REQUIRE(s->update_policy == "latest");
}

// ---------------------------------------------------------------------------
// build_executables
// ---------------------------------------------------------------------------

TEST_CASE("packer executables: mapping and skips", "[gmmpack][packer]") {
  TempDir td;
  nexus_mod(td.root, "ToolMod");
  manual_mod(td.root, "LocalMod");
  auto snap = make_snapshot();
  track(snap, "ToolMod", 0);
  track(snap, "LocalMod", 1);

  engine::ExecutableEntry good;
  good.path  = "tools/tool.exe";
  good.title = "Cool Tool";
  good.args  = "--headless --out dir";
  good.cwd   = "tools";
  good.mod   = "ToolMod";
  good.env   = {"FOO=bar", "BAZ=qux"};
  snap.executables.push_back(good);

  engine::ExecutableEntry game_exe;  // no source mod: skipped
  game_exe.path  = "SkyrimSE.exe";
  game_exe.title = "Game";
  snap.executables.push_back(game_exe);

  engine::ExecutableEntry manual_exe;  // manual mod: skipped
  manual_exe.path  = "local/run.exe";
  manual_exe.title = "Local";
  manual_exe.mod   = "LocalMod";
  snap.executables.push_back(manual_exe);

  auto execs = gmmpack::build_executables(snap, td.root);
  REQUIRE(execs.size() == 1);
  REQUIRE(execs[0].id == "cool-tool");
  REQUIRE(execs[0].source_mod_id == "toolmod");
  REQUIRE(execs[0].relative_path == "tools/tool.exe");
  REQUIRE(execs[0].role == "launcher");
  REQUIRE(execs[0].arguments == std::vector<std::string>{"--headless", "--out", "dir"});
  REQUIRE(execs[0].env_vars.at("FOO") == "bar");
  REQUIRE(execs[0].working_dir == "tools");
}

// ---------------------------------------------------------------------------
// build_gmmpack round-trip
// ---------------------------------------------------------------------------

TEST_CASE("packer round-trip: build then parse back", "[gmmpack][packer]") {
  TempDir td;
  nexus_mod(td.root, "SkyUI");
  auto snap = make_snapshot();
  track(snap, "SkyUI", 0);

  gmmpack::PackOptions opts;
  opts.author = "Author";
  auto pack   = gmmpack::build_gmmpack(snap, td.root, opts);

  // Manifest.
  auto mj = gmmpack::serialize_manifest(pack.manifest);
  auto m  = gmmpack::parse_manifest(mj);
  REQUIRE(m.id == pack.manifest.id);
  REQUIRE(m.info.name == "Test Pack");
  REQUIRE(m.info.author == "Author");

  // Mods.
  REQUIRE(pack.mods.size() == 1);
  auto modj = gmmpack::serialize_mod_entry(pack.mods[0]);
  auto mod  = gmmpack::parse_mod_entry(modj);
  REQUIRE(mod.id == "skyui");
  auto *n = std::get_if<gmmpack::ModSourceNexus>(&mod.source);
  REQUIRE(n != nullptr);
  REQUIRE(n->mod_id == 12345);

  // Tree.
  auto treej = gmmpack::serialize_tree(pack.tree);
  auto tree  = gmmpack::parse_tree(treej);
  REQUIRE(gmmpack::count_tree_mods(tree) == 1);
}

// ---------------------------------------------------------------------------
// create_gmmpack: archive validity
// ---------------------------------------------------------------------------

TEST_CASE("packer create: writes archive passing integrity check",
          "[gmmpack][packer]") {
  TempDir td;
  nexus_mod(td.root, "SkyUI");
  nexus_mod(td.root, "ENB");
  auto snap = make_snapshot();
  track(snap, "Graphics", 0, "", true);
  track(snap, "SkyUI", 1);
  track(snap, "ENB", 0, "Graphics");

  engine::ExecutableEntry tool;
  tool.path  = "tools/tool.exe";
  tool.title = "Tool";
  tool.mod   = "SkyUI";
  snap.executables.push_back(tool);

  gmmpack::PackOptions opts;
  opts.author       = "Author";
  opts.description  = "A pack";
  opts.instructions = "# Install\nDo the thing.\n";

  auto out = td.root / "test.gmmpack";
  auto res = gmmpack::create_gmmpack(snap, td.root, opts, out);
  REQUIRE(res.ok);
  REQUIRE(res.error.empty());
  REQUIRE(fs::exists(out));

  auto extract = gmmpack::extract_archive(out);
  REQUIRE(extract.ok);

  auto integrity =
      gmmpack::verify_archive_integrity(extract.archive, extract.manifest_json);
  REQUIRE(integrity.empty());

  auto manifest = gmmpack::parse_manifest(extract.manifest_json);
  REQUIRE(manifest.gmmpack_schema == "1.0.0");
  REQUIRE(manifest.revision == 1);
  REQUIRE(manifest.info.name == "Test Pack");

  // Expected payload files present.
  REQUIRE(extract.archive.path_index.count("mods/skyui.json"));
  REQUIRE(extract.archive.path_index.count("mods/enb.json"));
  REQUIRE(extract.archive.path_index.count("tree.json"));
  REQUIRE(extract.archive.path_index.count("executables/tool.json"));
  REQUIRE(extract.archive.path_index.count("instructions.md"));

  // Tree inside the archive parses and references real mods.
  auto tit  = extract.archive.path_index.find("tree.json");
  auto tree = gmmpack::parse_tree(
      nlohmann::json::parse(extract.archive.files[tit->second].content));
  auto flat = gmmpack::flatten_tree(tree);
  REQUIRE(flat.size() == 2);
}

// ---------------------------------------------------------------------------
// Schema validation of generated JSON
// ---------------------------------------------------------------------------

static fs::path schema_dir() {
  auto project_root = fs::path(PROJECT_SOURCE_DIR);
  auto candidate    = project_root / "schemas";
  if (fs::is_directory(candidate))
    return fs::canonical(candidate);
  FAIL("schema dir not found from " + project_root.string());
  return candidate;
}

TEST_CASE("packer schema: generated files validate", "[gmmpack][packer]") {
  TempDir td;
  nexus_mod(td.root, "SkyUI");

  auto meta = engine::ModMeta::from_default("M", "steam", "12345678");
  REQUIRE(gmmpack::resolve_mod_source(meta, "x", 1).has_value());
  auto ll = engine::ModMeta::from_default("M", "loverslab", "5");
  ll.set("LoversLab", "fileid", "5");
  REQUIRE(gmmpack::resolve_mod_source(ll, "x", 0).has_value());
  auto mp = engine::ModMeta::from_default("M", "modpub", "6");
  mp.set("ModPub", "mod_id", "6");
  REQUIRE(gmmpack::resolve_mod_source(mp, "x", 0).has_value());
  auto direct =
      engine::ModMeta::from_default("M", "direct", "https://example.com/a.zip");
  REQUIRE(gmmpack::resolve_mod_source(direct, "x", 0).has_value());

  auto snap = make_snapshot();
  track(snap, "SkyUI", 0);
  engine::ExecutableEntry tool;
  tool.path  = "tools/tool.exe";
  tool.title = "Tool";
  tool.mod   = "SkyUI";
  snap.executables.push_back(tool);

  gmmpack::PackOptions opts;
  opts.author = "Author";
  auto pack   = gmmpack::build_gmmpack(snap, td.root, opts);

  auto schemas = gmmpack::load_schema_set(schema_dir());
  gmmpack::SchemaValidator v;

  // Manifest needs fileHashes for schema (minProperties 1): fill from
  // the serialized payloads the way create_gmmpack does.
  auto modj  = gmmpack::serialize_mod_entry(pack.mods[0]);
  auto treej = gmmpack::serialize_tree(pack.tree);
  pack.manifest.archive.file_hashes["mods/skyui.json"] =
      "sha256:" + std::string(64, 'a');
  pack.manifest.archive.file_hashes["tree.json"] = "sha256:" + std::string(64, 'b');
  auto mj = gmmpack::serialize_manifest(pack.manifest);
  REQUIRE(v.validate(mj, schemas.at("manifest.schema.json"),
                     schemas.at("manifest.schema.json"))
              .empty());
  REQUIRE(v.validate(modj, schemas.at("mod.schema.json"), schemas.at("mod.schema.json"))
              .empty());
  REQUIRE(
      v.validate(treej, schemas.at("tree.schema.json"), schemas.at("tree.schema.json"))
          .empty());
  REQUIRE(v.validate(gmmpack::serialize_executable_entry(pack.executables[0]),
                     schemas.at("executable.schema.json"),
                     schemas.at("executable.schema.json"))
              .empty());

  // Every source variant serializes to schema-valid JSON.
  auto check_source = [&](const gmmpack::ModSource &s) {
    gmmpack::ModEntry e;
    e.id       = "probe-mod";
    e.name     = "Probe";
    e.category = gmmpack::ModCategory::Optional;
    e.source   = s;
    auto j     = gmmpack::serialize_mod_entry(e);
    auto diag =
        v.validate(j, schemas.at("mod.schema.json"), schemas.at("mod.schema.json"));
    INFO(j.dump());
    REQUIRE(diag.empty());
  };
  check_source(*gmmpack::resolve_mod_source(meta, "x", 1));
  check_source(*gmmpack::resolve_mod_source(ll, "x", 0));
  check_source(*gmmpack::resolve_mod_source(mp, "x", 0));
  check_source(*gmmpack::resolve_mod_source(direct, "x", 0));
  auto nx = engine::ModMeta::from_default("N", "nexus", "1");
  check_source(*gmmpack::resolve_mod_source(nx, "skyrim", 0));
}

// ---------------------------------------------------------------------------
// Bundled (embedded) mods: a manual mod must survive a round-trip through
// the importer instead of being dropped on export.
// ---------------------------------------------------------------------------

TEST_CASE("packer embedded: a bundled manual mod round-trips through import",
          "[gmmpack][packer]") {
  TempDir td;
  manual_mod(td.root, "LocalMod");
  fs::create_directories(td.root / "LocalMod" / "scripts");
  {
    std::ofstream esp(td.root / "LocalMod" / "main.esp", std::ios::binary);
    esp << "ESP DATA";
  }
  {
    std::ofstream pex(td.root / "LocalMod" / "scripts" / "foo.pex", std::ios::binary);
    pex << "PEX DATA";
  }

  auto snap = make_snapshot();
  track(snap, "LocalMod", 0);

  gmmpack::PackOptions opts;
  opts.author     = "Author";
  opts.schema_dir = schema_dir();
  opts.embed_folders.insert("LocalMod");

  const auto out = td.root / "embedded.gmmpack";
  const auto res = gmmpack::create_gmmpack(snap, td.root, opts, out);
  REQUIRE(res.ok);
  REQUIRE(res.error.empty());
  REQUIRE(res.embedded_mod_count == 1);
  REQUIRE(res.embedded_file_count == 3);  // meta.ini + main.esp + scripts/foo.pex
  REQUIRE(fs::exists(out));

  // The payload and its per-file hashes are in the archive.
  auto extract = gmmpack::extract_archive(out);
  REQUIRE(extract.ok);
  REQUIRE(extract.archive.path_index.count("files/localmod/main.esp"));
  REQUIRE(extract.archive.path_index.count("files/localmod/scripts/foo.pex"));
  REQUIRE(gmmpack::verify_archive_integrity(extract.archive, extract.manifest_json)
              .empty());

  // Import accepts it, and the mod entry points at the payload.
  auto unpacked = gmmpack::unpack_gmmpack(out, schema_dir());
  INFO([&] {
    std::string joined;
    for (const auto &d : unpacked.diagnostics)
      joined += d.path + ": " + d.message + "\n";
    return joined;
  }());
  REQUIRE(unpacked.ok);
  REQUIRE(unpacked.pack.mods.size() == 1);
  const auto *mod = gmmpack::find_mod(unpacked.pack, "localmod");
  REQUIRE(mod != nullptr);
  const auto *source = std::get_if<gmmpack::ModSourceEmbedded>(&mod->source);
  REQUIRE(source != nullptr);
  REQUIRE(source->root == "localmod");
  REQUIRE(source->files.size() == 3);
  const auto esp = std::find_if(source->files.begin(), source->files.end(),
                                [](const gmmpack::ModSourceEmbedded::File &f) {
                                  return f.path == "main.esp";
                                });
  REQUIRE(esp != source->files.end());
  REQUIRE(esp->size == 8);
  REQUIRE(esp->sha256.size() == 64);

  // Installing it writes the exact bytes back out.
  const auto dest = td.root / "installed" / "localmod";
  std::string error;
  REQUIRE(gmmpack::extract_embedded_mod(unpacked.pack, "localmod", dest, error));
  REQUIRE(error.empty());
  std::ifstream esp_in(dest / "main.esp", std::ios::binary);
  const std::string content((std::istreambuf_iterator<char>(esp_in)),
                            std::istreambuf_iterator<char>());
  REQUIRE(content == "ESP DATA");
  std::ifstream pex_in(dest / "scripts" / "foo.pex", std::ios::binary);
  const std::string pex_content((std::istreambuf_iterator<char>(pex_in)),
                                std::istreambuf_iterator<char>());
  REQUIRE(pex_content == "PEX DATA");

  // A tampered payload is refused instead of installed.
  auto tampered = unpacked.pack;
  for (auto &af : tampered.payload) {
    if (af.path == "files/localmod/main.esp")
      af.content = "TAMPERED";
  }
  const auto tampered_dest = td.root / "tampered" / "localmod";
  REQUIRE_FALSE(
      gmmpack::extract_embedded_mod(tampered, "localmod", tampered_dest, error));
  REQUIRE(!error.empty());
  REQUIRE_FALSE(fs::exists(tampered_dest / "main.esp"));
}

// ---------------------------------------------------------------------------
// Exact version pinning: a pack GMM cannot reopen is not an option, so the
// pin is only claimed when its download identity resolves.
// ---------------------------------------------------------------------------

TEST_CASE("packer policies: exact pin imports when the download identity is known",
          "[gmmpack][packer]") {
  TempDir td;
  // What InstallStage stamps at download time: the archive's own size + hash.
  auto meta = engine::ModMeta::from_default("SkyUI", "nexus", "12345", "SkyUI-1.4.2.7z",
                                            "1.4.2");
  meta.set("Nexusmods", "modid", "12345");
  meta.set("Nexusmods", "fileid", "67890");
  meta.set("GameModManager", "download_size", "12345");
  meta.set("GameModManager", "download_sha256", std::string(64, 'a'));
  write_meta(td.root, "SkyUI", meta);

  auto snap = make_snapshot();
  track(snap, "SkyUI", 0);

  gmmpack::PackOptions opts;
  opts.author                   = "Author";
  opts.schema_dir               = schema_dir();
  opts.update_policies["SkyUI"] = "exact";

  const auto out = td.root / "exact.gmmpack";
  const auto res = gmmpack::create_gmmpack(snap, td.root, opts, out);
  REQUIRE(res.ok);
  REQUIRE(res.error.empty());

  // The whole point: GMM opens its own pack.
  auto unpacked = gmmpack::unpack_gmmpack(out, schema_dir());
  INFO([&] {
    std::string joined;
    for (const auto &d : unpacked.diagnostics)
      joined += d.path + ": " + d.message + "\n";
    return joined;
  }());
  REQUIRE(unpacked.ok);

  const auto *source =
      std::get_if<gmmpack::ModSourceNexus>(&unpacked.pack.mods[0].source);
  REQUIRE(source != nullptr);
  REQUIRE(source->update_policy == "exact");
  REQUIRE(source->file_size.has_value());
  REQUIRE(*source->file_size == 12345);
  REQUIRE(source->sha256.has_value());
  REQUIRE(*source->sha256 == std::string(64, 'a'));

  // Installed before that stamp existed: the same identity is recovered by
  // hashing the archive still sitting in the instance's downloads folder.
  auto legacy = engine::ModMeta::from_default("SkyUI", "nexus", "12345",
                                              "SkyUI-1.4.2.7z", "1.4.2");
  legacy.set("Nexusmods", "modid", "12345");
  legacy.set("Nexusmods", "fileid", "67890");
  write_meta(td.root, "SkyUI", legacy);
  const auto downloads = td.root / "downloads";
  fs::create_directories(downloads);
  {
    std::ofstream archive(downloads / "SkyUI-1.4.2.7z", std::ios::binary);
    archive << "ARCHIVE BYTES";
  }
  gmmpack::PackOptions backfill;
  backfill.author                   = "Author";
  backfill.schema_dir               = schema_dir();
  backfill.downloads_dir            = downloads;
  backfill.update_policies["SkyUI"] = "exact";
  const auto backfilled_out         = td.root / "backfilled.gmmpack";
  const auto backfilled =
      gmmpack::create_gmmpack(snap, td.root, backfill, backfilled_out);
  REQUIRE(backfilled.ok);
  auto backfilled_pack = gmmpack::unpack_gmmpack(backfilled_out, schema_dir());
  REQUIRE(backfilled_pack.ok);
  const auto *recovered =
      std::get_if<gmmpack::ModSourceNexus>(&backfilled_pack.pack.mods[0].source);
  REQUIRE(recovered != nullptr);
  REQUIRE(recovered->update_policy == "exact");
  REQUIRE(recovered->file_size.has_value());
  REQUIRE(*recovered->file_size == 13);
  REQUIRE(recovered->sha256.has_value());
}

TEST_CASE("packer policies: unresolvable identity degrades to latest, not a broken pin",
          "[gmmpack][packer]") {
  TempDir td;
  nexus_mod(td.root, "SkyUI");  // no download_size / download_sha256
  auto snap = make_snapshot();
  track(snap, "SkyUI", 0);

  gmmpack::PackOptions opts;
  opts.author                   = "Author";
  opts.schema_dir               = schema_dir();
  opts.update_policies["SkyUI"] = "exact";

  const auto out = td.root / "degraded.gmmpack";
  const auto res = gmmpack::create_gmmpack(snap, td.root, opts, out);
  REQUIRE(res.ok);
  REQUIRE(res.error.empty());

  auto unpacked = gmmpack::unpack_gmmpack(out, schema_dir());
  REQUIRE(unpacked.ok);
  const auto *source =
      std::get_if<gmmpack::ModSourceNexus>(&unpacked.pack.mods[0].source);
  REQUIRE(source != nullptr);
  REQUIRE(source->update_policy == "latest");
  REQUIRE_FALSE(source->sha256.has_value());
}

// ---------------------------------------------------------------------------
// create_gmmpack runs the importer's own validation on what it built.
// ---------------------------------------------------------------------------

TEST_CASE("packer create: an invalid pack is refused, not written",
          "[gmmpack][packer]") {
  TempDir td;
  nexus_mod(td.root, "SkyUI");
  auto snap = make_snapshot();
  track(snap, "SkyUI", 0);

  gmmpack::PackOptions opts;
  opts.author     = "Author";
  opts.schema_dir = schema_dir();
  // A choice group naming a mod that is not in the pack: exactly the kind of
  // author error the export must not ship.
  gmmpack::ChoiceGroup group;
  group.id             = "textures";
  group.name           = "Textures";
  group.mode           = "at-most-one";
  group.member_mod_ids = {"does-not-exist", "skyui"};
  opts.choice_groups.push_back(group);

  const auto out = td.root / "invalid.gmmpack";
  const auto res = gmmpack::create_gmmpack(snap, td.root, opts, out);
  INFO("create_gmmpack error: " << res.error);
  REQUIRE_FALSE(res.ok);
  REQUIRE(res.error.find("did not validate") != std::string::npos);
  REQUIRE(res.error.find("does-not-exist") != std::string::npos);
  REQUIRE_FALSE(fs::exists(out));
}

// ---------------------------------------------------------------------------
// ini/ producer and colliding paths: a shipped INI becomes payload, while two
// mods shipping the same path do NOT become a binary patch. The pack references
// both mods and the deploy order in tree.json decides who wins the file.
// ---------------------------------------------------------------------------

TEST_CASE("packer ini+collision: shipped INI becomes payload, colliding files "
          "are left to deploy order",
          "[gmmpack][packer]") {
  TempDir td;
  nexus_mod(td.root, "Clothes");
  nexus_mod(td.root, "ENB");
  // Both mods ship textures/example.dds with different bytes. ENB is lower in
  // the list, so at install it loses that path to Clothes and the pack carries
  // no delta for it.
  fs::create_directories(td.root / "Clothes" / "textures");
  fs::create_directories(td.root / "ENB" / "textures");
  {
    std::ofstream winner(td.root / "Clothes" / "textures" / "example.dds",
                         std::ios::binary);
    winner << std::string(4096, 'A');
  }
  {
    std::ofstream loser(td.root / "ENB" / "textures" / "example.dds", std::ios::binary);
    loser << std::string(4096, 'A');
    loser << std::string(4096, 'B');
  }
  // A mod's own INI file becomes one toggleable tweak.
  {
    std::ofstream ini(td.root / "ENB" / "ENB.ini", std::ios::binary);
    ini << "[ENB]\niAA=8\n";
  }

  auto snap = make_snapshot();
  track(snap, "Clothes", 0);
  track(snap, "ENB", 1);

  gmmpack::PackOptions opts;
  opts.author     = "Author";
  opts.schema_dir = schema_dir();

  const auto out = td.root / "payload.gmmpack";
  const auto res = gmmpack::create_gmmpack(snap, td.root, opts, out);
  INFO("create_gmmpack error: " << res.error);
  REQUIRE(res.ok);

  auto extract = gmmpack::extract_archive(out);
  REQUIRE(extract.ok);
  REQUIRE(extract.archive.path_index.count("ini/ENB.ini.json"));
  // Both mods are still in the pack; neither was dropped for colliding.
  REQUIRE(extract.archive.path_index.count("mods/clothes.json"));
  REQUIRE(extract.archive.path_index.count("mods/enb.json"));
  // No patch directory, and no base64 delta in any JSON the pack carries.
  for (const auto &entry : extract.archive.files) {
    INFO("unexpected archive entry: " << entry.path);
    REQUIRE(entry.path.rfind("patches/", 0) != 0);
    if (entry.path.ends_with(".json"))
      REQUIRE(entry.content.find("payloadBase64") == std::string::npos);
  }
  const auto manifest = gmmpack::parse_manifest(extract.manifest_json);
  REQUIRE(manifest.archive.file_hashes.count("patches/enb.json") == 0);

  // The pack is still schema-valid, referentially sound, and importable.
  auto unpacked = gmmpack::unpack_gmmpack(out, schema_dir());
  INFO([&] {
    std::string joined;
    for (const auto &d : unpacked.diagnostics)
      joined += d.path + ": " + d.message + "\n";
    return joined;
  }());
  REQUIRE(unpacked.ok);
  REQUIRE(unpacked.pack.ini_edits.size() == 1);
  REQUIRE(unpacked.pack.ini_edits[0].target_file == "ENB.ini");
  REQUIRE(unpacked.pack.ini_edits[0].tweaks.size() == 1);
  REQUIRE(unpacked.pack.ini_edits[0].tweaks[0].source_mod_id == "enb");
  REQUIRE(unpacked.pack.ini_edits[0].tweaks[0].content.find("iAA=8") !=
          std::string::npos);
  REQUIRE(unpacked.pack.patches.empty());
  REQUIRE(unpacked.pack.mods.size() == 2);

  // What resolves the collision is the deploy order the tree carries: ENB
  // comes after Clothes, so Clothes' copy of textures/example.dds wins.
  REQUIRE(unpacked.pack.tree.nodes.size() == 2);
  const auto *first  = std::get_if<gmmpack::ModNode>(&unpacked.pack.tree.nodes[0].data);
  const auto *second = std::get_if<gmmpack::ModNode>(&unpacked.pack.tree.nodes[1].data);
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  REQUIRE(first->id == "clothes");
  REQUIRE(second->id == "enb");
}
