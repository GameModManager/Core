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
//   - build_executables mapping + unresolvable skipped
//   - build_gmmpack round-trip (serialize -> parse_manifest/parse_mod_entry/
//     parse_tree back)
//   - create_gmmpack writes an archive that passes extract + integrity check
//   - generated manifest/mod JSON validates against the input/ schemas

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

struct TempDir
{
  fs::path root;
  TempDir()
  {
    static int counter = 0;
    root               = fs::temp_directory_path() /
                         ("gmmpack_packer_test_" + std::to_string(::getpid()) + "_" +
                          std::to_string(counter++));
    fs::create_directories(root);
  }
  ~TempDir()
  {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

static void write_meta(const fs::path& mods_dir, const std::string& folder,
                       engine::ModMeta meta)
{
  meta.set("GameModManager", "folder", folder);
  REQUIRE(meta.save(mods_dir, folder));
}

static void nexus_mod(const fs::path& mods_dir, const std::string& folder,
                      const std::string& mod_id = "12345",
                      const std::string& fileid = "67890")
{
  auto meta =
      engine::ModMeta::from_default(folder, "nexus", mod_id, "SkyUI-1.4.2.7z", "1.4.2");
  meta.set("Nexusmods", "modid", mod_id);
  meta.set("Nexusmods", "mod_id", mod_id);
  meta.set("Nexusmods", "fileid", fileid);
  write_meta(mods_dir, folder, meta);
}

static void manual_mod(const fs::path& mods_dir, const std::string& folder)
{
  write_meta(mods_dir, folder, engine::ModMeta::from_default(folder, "manual", ""));
}

static engine::InstanceSnapshot make_snapshot()
{
  engine::InstanceSnapshot snap;
  snap.game_id      = "skyrimspecialedition";
  snap.display_name = "Test Pack";
  snap.steam_appid  = 489830;
  return snap;
}

static void track(engine::InstanceSnapshot& snap, const std::string& folder,
                  int32_t pos, const std::string& parent = "", bool collapsed = false,
                  bool hidden = false, bool disabled = false)
{
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

TEST_CASE("packer resolve: nexus", "[gmmpack][packer]")
{
  TempDir td;
  nexus_mod(td.root, "SkyUI");
  auto meta = engine::ModMeta::load(td.root, "SkyUI");

  auto src = gmmpack::resolve_mod_source(meta, "skyrimspecialedition", 489830);
  REQUIRE(src.has_value());
  auto* n = std::get_if<gmmpack::ModSourceNexus>(&*src);
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
          "[gmmpack][packer]")
{
  auto meta = engine::ModMeta::from_default("M", "nexus", "999", "", "2.0");
  auto src  = gmmpack::resolve_mod_source(meta, "skyrim", 0);
  REQUIRE(src.has_value());
  auto* n = std::get_if<gmmpack::ModSourceNexus>(&*src);
  REQUIRE(n != nullptr);
  REQUIRE(n->resolution == "browser");
  REQUIRE_FALSE(n->file_id.has_value());
}

TEST_CASE("packer resolve: steam", "[gmmpack][packer]")
{
  auto meta = engine::ModMeta::from_default("M", "steam", "12345678", "", "1.0");
  auto src  = gmmpack::resolve_mod_source(meta, "skyrim", 489830);
  REQUIRE(src.has_value());
  auto* s = std::get_if<gmmpack::ModSourceSteamWorkshop>(&*src);
  REQUIRE(s != nullptr);
  REQUIRE(s->workshop_item_id == 12345678);
  REQUIRE(s->app_id == 489830);
  REQUIRE(s->update_policy == "latest");
}

TEST_CASE("packer resolve: steam_workshop alias", "[gmmpack][packer]")
{
  auto meta = engine::ModMeta::from_default("M", "steam_workshop", "42");
  auto src  = gmmpack::resolve_mod_source(meta, "skyrim", 72850);
  REQUIRE(src.has_value());
  auto* s = std::get_if<gmmpack::ModSourceSteamWorkshop>(&*src);
  REQUIRE(s != nullptr);
  REQUIRE(s->workshop_item_id == 42);
  REQUIRE(s->app_id == 72850);
}

TEST_CASE("packer resolve: loverslab", "[gmmpack][packer]")
{
  auto meta = engine::ModMeta::from_default("M", "loverslab", "777", "", "3.0");
  meta.set("LoversLab", "fileid", "777");
  meta.set("LoversLab", "page_url", "https://www.loverslab.com/files/file/777/");
  auto src = gmmpack::resolve_mod_source(meta, "skyrim", 0);
  REQUIRE(src.has_value());
  auto* l = std::get_if<gmmpack::ModSourceLoversLab>(&*src);
  REQUIRE(l != nullptr);
  REQUIRE(std::get<int64_t>(l->mod_id) == 777);
  REQUIRE(l->resolution == "browser");
}

TEST_CASE("packer resolve: modpub", "[gmmpack][packer]")
{
  auto meta = engine::ModMeta::from_default("M", "modpub", "99", "", "1.1");
  meta.set("ModPub", "mod_id", "99");
  auto src = gmmpack::resolve_mod_source(meta, "skyrim", 0);
  REQUIRE(src.has_value());
  auto* p = std::get_if<gmmpack::ModSourceModPub>(&*src);
  REQUIRE(p != nullptr);
  REQUIRE(std::get<int64_t>(p->mod_id) == 99);
}

TEST_CASE("packer resolve: direct", "[gmmpack][packer]")
{
  auto meta = engine::ModMeta::from_default(
      "M", "direct", "https://example.com/mods/a.zip", "", "1.0");
  auto src = gmmpack::resolve_mod_source(meta, "skyrim", 0);
  REQUIRE(src.has_value());
  auto* d = std::get_if<gmmpack::ModSourceDirect>(&*src);
  REQUIRE(d != nullptr);
  REQUIRE(d->url == "https://example.com/mods/a.zip");
}

TEST_CASE("packer resolve: manual is skipped", "[gmmpack][packer]")
{
  auto meta = engine::ModMeta::from_default("M", "manual", "");
  REQUIRE_FALSE(gmmpack::resolve_mod_source(meta, "skyrim", 0).has_value());
}

TEST_CASE("packer resolve: unknown type is skipped", "[gmmpack][packer]")
{
  auto meta = engine::ModMeta::from_default("M", "bogus", "x");
  REQUIRE_FALSE(gmmpack::resolve_mod_source(meta, "skyrim", 0).has_value());
}

// ---------------------------------------------------------------------------
// mod_slug
// ---------------------------------------------------------------------------

TEST_CASE("packer slug: mapping", "[gmmpack][packer]")
{
  REQUIRE(gmmpack::mod_slug("SkyUI") == "skyui");
  REQUIRE(gmmpack::mod_slug("My Cool Mod!") == "my-cool-mod");
  REQUIRE(gmmpack::mod_slug("a__b--c") == "a-b-c");
  REQUIRE(gmmpack::mod_slug("") == "mod");
  REQUIRE(gmmpack::mod_slug("---") == "mod");
}

// ---------------------------------------------------------------------------
// build_tree
// ---------------------------------------------------------------------------

TEST_CASE("packer tree: flat list sorted by position", "[gmmpack][packer]")
{
  TempDir td;
  nexus_mod(td.root, "ModB");
  nexus_mod(td.root, "ModA");
  auto snap = make_snapshot();
  track(snap, "ModB", 0);
  track(snap, "ModA", 1);

  auto tree = gmmpack::build_tree(snap, td.root);
  REQUIRE(tree.nodes.size() == 2);
  auto* first  = std::get_if<gmmpack::ModNode>(&tree.nodes[0].data);
  auto* second = std::get_if<gmmpack::ModNode>(&tree.nodes[1].data);
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  REQUIRE(first->id == "modb");
  REQUIRE(second->id == "moda");
  REQUIRE(first->enabled);
}

TEST_CASE("packer tree: nested separators", "[gmmpack][packer]")
{
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
  auto* sep = std::get_if<gmmpack::SeparatorNode>(&tree.nodes[0].data);
  REQUIRE(sep != nullptr);
  REQUIRE(sep->name == "Graphics");
  REQUIRE(sep->collapsed);
  REQUIRE(sep->children.size() == 1);
  auto* child = std::get_if<gmmpack::ModNode>(&sep->children[0].data);
  REQUIRE(child != nullptr);
  REQUIRE(child->id == "enb");
  auto* top = std::get_if<gmmpack::ModNode>(&tree.nodes[1].data);
  REQUIRE(top != nullptr);
  REQUIRE(top->id == "skyui");
}

TEST_CASE("packer tree: hidden/disabled mods marked not enabled", "[gmmpack][packer]")
{
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

TEST_CASE("packer tree: manual mods omitted", "[gmmpack][packer]")
{
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

// ---------------------------------------------------------------------------
// build_manifest
// ---------------------------------------------------------------------------

TEST_CASE("packer manifest: fields", "[gmmpack][packer]")
{
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
          "[gmmpack][packer]")
{
  auto snap             = make_snapshot();
  snap.modpack_id       = "11111111-2222-4333-8444-555555555555";
  snap.modpack_revision = 4;
  gmmpack::PackOptions opts;

  auto m = gmmpack::build_manifest(snap, opts);
  REQUIRE(m.id == "11111111-2222-4333-8444-555555555555");
  REQUIRE(m.revision == 5);
}

TEST_CASE("packer manifest: fallbacks for empty fields", "[gmmpack][packer]")
{
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

// ---------------------------------------------------------------------------
// build_mod_entries
// ---------------------------------------------------------------------------

TEST_CASE("packer mod entries: skips manual, deterministic order", "[gmmpack][packer]")
{
  TempDir td;
  nexus_mod(td.root, "Zeta");
  manual_mod(td.root, "Local");
  nexus_mod(td.root, "Alpha");
  auto snap = make_snapshot();
  track(snap, "Zeta", 1);
  track(snap, "Local", 2);
  track(snap, "Alpha", 0);

  auto mods = gmmpack::build_mod_entries(snap, td.root);
  REQUIRE(mods.size() == 2);
  REQUIRE(mods[0].id == "alpha");
  REQUIRE(mods[1].id == "zeta");
  REQUIRE(mods[0].name == "Alpha");
  REQUIRE(mods[0].category == gmmpack::ModCategory::Optional);
  REQUIRE(mods[0].phase == 0);
}

// ---------------------------------------------------------------------------
// build_executables
// ---------------------------------------------------------------------------

TEST_CASE("packer executables: mapping and skips", "[gmmpack][packer]")
{
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

TEST_CASE("packer round-trip: build then parse back", "[gmmpack][packer]")
{
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
  auto* n = std::get_if<gmmpack::ModSourceNexus>(&mod.source);
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

TEST_CASE("packer create: writes archive passing integrity check", "[gmmpack][packer]")
{
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

static fs::path schema_dir()
{
  auto project_root = fs::path(PROJECT_SOURCE_DIR);
  auto candidate    = project_root / ".." / ".." / "input";
  if (fs::is_directory(candidate))
    return fs::canonical(candidate);
  candidate = project_root / ".." / "input";
  if (fs::is_directory(candidate))
    return fs::canonical(candidate);
  FAIL("schema dir not found from " + project_root.string());
  return candidate;
}

TEST_CASE("packer schema: generated files validate", "[gmmpack][packer]")
{
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
  auto check_source = [&](const gmmpack::ModSource& s) {
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
