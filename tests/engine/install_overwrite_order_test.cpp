// A Replace overwrite must not cost the mod its place in the list.
//
// OverwriteAction::Replace deletes the mod folder and re-creates it empty
// (install_stage.cpp:213-236), and meta.ini goes with it. Everything the
// install stamps afterwards comes from ModMeta::from_default, which writes
// neither "priority" nor "separator_id" - so before this was carried across,
// both keys were simply gone: ModMeta::priority() reads -1 for an absent key,
// which makes install_stage's own `if (existing_priority >= 0)` branch
// unreachable on a Replace, and the mod list's next load appends the row at the
// bottom of the user band (mod_list_model.cpp:1184) with no separator. The
// user pressed "overwrite the existing install" and lost their load order.
//
// MO2 keeps the whole of meta.ini across the delete and restores it
// (installationmanager.cpp:417-447). These cases pin the two keys that carry
// the mod's position, and pin that the archive identity is NOT carried - a
// Replace that restored the old installationfile would leave the new install
// unrecorded.
//
// Hermetic: throwaway temp root, real InstallStage, no UI, no network.
#include "engine/mod/meta/mod_meta.h"
#include "engine/mod/model/mod.h"
#include "engine/pipeline/install_stage.h"
#include "engine/pipeline/pipeline.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

using namespace engine;

namespace {

struct TempDir {
  explicit TempDir(const std::string &name)
      : root(fs::temp_directory_path() /
             ("gmm_overwrite_order_" + name + "_" + std::to_string(::getpid()) + "_" +
              std::to_string(counter_++))) {
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  TempDir(const TempDir &)            = delete;
  TempDir &operator=(const TempDir &) = delete;
  fs::path root;
  static int counter_;
};
int TempDir::counter_ = 0;

// The extracted tree InstallStage copies from. Its name never reaches the mod
// folder: InstallStage names the destination from `mod.name`.
fs::path seed_staging(const fs::path &root, const std::string &content = "v2 payload") {
  const fs::path staging = root / "MetaMod_staging";
  fs::create_directories(staging / "meshes");
  std::ofstream(staging / "meshes" / "thing.nif") << content;
  return staging;
}

// An existing install with a real position in the list: priority 17 and a
// separator membership, both written by the app's own key names.
void seed_installed(const fs::path &mods_dir, const std::string &folder,
                    const std::string &old_archive) {
  fs::create_directories(mods_dir / folder / "meshes");
  std::ofstream(mods_dir / folder / "meshes" / "thing.nif") << "v1 payload";
  ModMeta meta;
  meta.set("General", "installationfile", old_archive);
  meta.set("General", "installed", "2026-01-01T00:00:00");
  meta.set("GameModManager", "folder", folder);
  meta.set("GameModManager", "separator_id", "01.1 EARLY LOADERS - CORE_separator");
  meta.set_priority(17);
  REQUIRE(meta.save_file(mods_dir / folder / "meta.ini"));
}

}  // namespace

TEST_CASE("an overwrite keeps the mod's load-order position",
          "[engine][install][overwrite]") {
  TempDir tmp("priority");
  const fs::path mods_dir = tmp.root / "mods";
  fs::create_directories(mods_dir);
  seed_installed(mods_dir, "MetaMod", "MetaMod 59818 1.24.0 old.7z");

  Mod mod;
  mod.id                   = "59818-792670";
  mod.name                 = "MetaMod";
  mod.state                = ModState::Extracted;
  mod.download_source_type = "nexus";
  mod.download_source_id   = "59818";
  mod.archive_filename     = "MetaMod 59818 1.25.0 new.7z";
  ModFile staged;
  staged.relative_path = seed_staging(tmp.root).string();
  mod.files.push_back(staged);

  PipelineContext ctx;
  ctx.mods_dir           = mods_dir.string();
  ctx.overwrite_query_cb = [](const std::string &) {
    OverwriteDecision d;
    d.action = OverwriteAction::Replace;
    return d;
  };

  InstallStage stage;
  REQUIRE(stage.execute(mod, ctx));
  REQUIRE(ctx.error_message.empty());
  // The Replace really happened - otherwise this case would pass on the Merge
  // path, where the old meta.ini is still on disk and nothing needed carrying.
  REQUIRE(ctx.replaced_archive == "MetaMod 59818 1.24.0 old.7z");

  const ModMeta after = ModMeta::load_file(mods_dir / "MetaMod" / "meta.ini");
  // The assertion the user sees: the row comes back where it was, not at the
  // bottom of the list. priority() reads -1 when the key is absent, which is
  // the whole bug - so compare against the number, never against "> 0".
  CHECK(after.priority() == 17);
  CHECK(after.separator_id() == "01.1 EARLY LOADERS - CORE_separator");

  // ...and the install that just ran is the one now recorded. Carrying the
  // whole of meta.ini the way MO2 does would keep the OLD archive here and
  // make the mod claim it was installed from a file that is no longer the one
  // on disk.
  CHECK(after.get("General", "installationfile") == "MetaMod 59818 1.25.0 new.7z");
  // The new payload landed, so the position above describes the new install.
  CHECK(fs::exists(mods_dir / "MetaMod" / "meshes" / "thing.nif"));
}

TEST_CASE("a merge still keeps the position, and an uninstalled mod invents none",
          "[engine][install][overwrite]") {
  // Merge keeps the old meta.ini, so the carry must not override what is
  // already there.
  TempDir merged("merge");
  const fs::path merged_mods = merged.root / "mods";
  fs::create_directories(merged_mods);
  seed_installed(merged_mods, "MetaMod", "MetaMod 1.24.0 old.7z");

  Mod mod;
  mod.id               = "MetaMod";
  mod.name             = "MetaMod";
  mod.state            = ModState::Extracted;
  mod.archive_filename = "MetaMod 1.25.0 new.7z";
  ModFile staged;
  staged.relative_path = seed_staging(merged.root, "v2 merge payload").string();
  mod.files.push_back(staged);

  PipelineContext merge_ctx;
  merge_ctx.mods_dir           = merged_mods.string();
  merge_ctx.overwrite_query_cb = [](const std::string &) {
    OverwriteDecision d;
    d.action = OverwriteAction::Merge;
    return d;
  };

  InstallStage stage;
  REQUIRE(stage.execute(mod, merge_ctx));
  CHECK(ModMeta::load_file(merged_mods / "MetaMod" / "meta.ini").priority() == 17);

  // A fresh install into a folder with nothing in it has no position to keep
  // and must not acquire one from the (unset) carry.
  TempDir fresh("fresh");
  const fs::path fresh_mods = fresh.root / "mods";
  fs::create_directories(fresh_mods);
  Mod fresh_mod;
  fresh_mod.id               = "BrandNew";
  fresh_mod.name             = "BrandNew";
  fresh_mod.state            = ModState::Extracted;
  fresh_mod.archive_filename = "BrandNew-1-1.7z";
  ModFile fresh_staged;
  fresh_staged.relative_path = seed_staging(fresh.root, "brand new").string();
  fresh_mod.files.push_back(fresh_staged);

  PipelineContext fresh_ctx;
  fresh_ctx.mods_dir = fresh_mods.string();
  InstallStage fresh_stage;
  REQUIRE(fresh_stage.execute(fresh_mod, fresh_ctx));
  // -1, not 0: a priority of 0 would pin the mod to the very top of the list.
  CHECK(ModMeta::load_file(fresh_mods / "BrandNew" / "meta.ini").priority() == -1);
  CHECK(
      ModMeta::load_file(fresh_mods / "BrandNew" / "meta.ini").separator_id().empty());
}