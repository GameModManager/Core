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
#include "engine/source/nexus/update_check.h"

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

// ---------------------------------------------------------------------------
// The disable sentinel. A Replace must not turn a disabled mod back on.
//
// The sentinel is a FILE inside the mod folder and it is the only record of a
// mod being OFF that the deploy filter consults: deploy_impl skips a folder
// whose sentinel exists and reads nothing else (deploy_utils.cpp:389), so the
// per-profile modlist.txt cannot rescue it. OverwriteAction::Replace deletes
// the folder and re-creates it empty, and nothing re-created the sentinel - so
// "update the mod" silently enabled it, in the deploy set and therefore in the
// load order, for a mod the user had deliberately turned off.
//
// MO2's Replace (installationmanager.cpp:417-447) saves meta.ini around the
// delete and restores it but likewise does not save the disabled file, so MO2
// leans on its profile's modlist.txt plus its own refresh. We have to carry
// the sentinel ourselves, which is what these cases pin.
//
// Hermetic: throwaway temp root, real InstallStage, no UI, no network.
TEST_CASE("an overwrite keeps a disabled mod disabled",
          "[engine][install][overwrite][enabled]") {
  // The engine's own default mechanism, resolved by the caller from
  // disable_mechanism_for() and handed over in the context - named here so a
  // hardcoded ".gmmdisabled" in the stage would fail rather than pass.
  const std::string sentinel = "disable.it";

  const auto run_replace = [&](const fs::path &mods_dir, const fs::path &root) {
    Mod mod;
    mod.id               = "59818-792670";
    mod.name             = "MetaMod";
    mod.state            = ModState::Extracted;
    mod.archive_filename = "MetaMod 1.25.0 new.7z";
    ModFile staged;
    staged.relative_path = seed_staging(root).string();
    mod.files.push_back(staged);

    PipelineContext ctx;
    ctx.mods_dir           = mods_dir.string();
    ctx.disable_mechanism  = sentinel;
    ctx.overwrite_query_cb = [](const std::string &) {
      OverwriteDecision d;
      d.action = OverwriteAction::Replace;
      return d;
    };
    InstallStage stage;
    REQUIRE(stage.execute(mod, ctx));
    // The delete really happened, so this is not the Merge path where the
    // sentinel survives untouched.
    REQUIRE(ctx.replaced_archive == "MetaMod 1.24.0 old.7z");
  };

  SECTION("disabled before the update, still disabled after it") {
    TempDir tmp("disabled");
    const fs::path mods_dir = tmp.root / "mods";
    fs::create_directories(mods_dir);
    seed_installed(mods_dir, "MetaMod", "MetaMod 1.24.0 old.7z");
    // Exactly what ModScanner::disable_mod writes: an empty file named by the
    // game's disable_mechanism.
    std::ofstream(mods_dir / "MetaMod" / sentinel);
    REQUIRE(fs::exists(mods_dir / "MetaMod" / sentinel));

    run_replace(mods_dir, tmp.root);

    INFO("mods dir after the replace: ");
    for (const auto &e : fs::directory_iterator(mods_dir / "MetaMod"))
      INFO("  " << e.path().filename().string());
    CHECK(fs::exists(mods_dir / "MetaMod" / sentinel));
    // The content moved, so the folder is genuinely the new install and the
    // sentinel is not surviving by the delete never having happened.
    CHECK(fs::exists(mods_dir / "MetaMod" / "meshes" / "thing.nif"));
  }

  SECTION("enabled before the update, and no sentinel is invented") {
    TempDir tmp("enabled");
    const fs::path mods_dir = tmp.root / "mods";
    fs::create_directories(mods_dir);
    seed_installed(mods_dir, "MetaMod", "MetaMod 1.24.0 old.7z");
    REQUIRE_FALSE(fs::exists(mods_dir / "MetaMod" / sentinel));

    run_replace(mods_dir, tmp.root);

    // The other half of the same assertion. A carry that wrote the sentinel
    // unconditionally would disable every mod on its first update - which is
    // the same class of wrong answer, in the opposite direction.
    CHECK_FALSE(fs::exists(mods_dir / "MetaMod" / sentinel));
  }

  SECTION("merge keeps the sentinel without touching it") {
    TempDir tmp("merge_disabled");
    const fs::path mods_dir = tmp.root / "mods";
    fs::create_directories(mods_dir);
    seed_installed(mods_dir, "MetaMod", "MetaMod 1.24.0 old.7z");
    std::ofstream(mods_dir / "MetaMod" / sentinel);

    Mod mod;
    mod.id               = "MetaMod";
    mod.name             = "MetaMod";
    mod.state            = ModState::Extracted;
    mod.archive_filename = "MetaMod 1.25.0 new.7z";
    ModFile staged;
    staged.relative_path = seed_staging(tmp.root, "merged").string();
    mod.files.push_back(staged);

    PipelineContext ctx;
    ctx.mods_dir           = mods_dir.string();
    ctx.disable_mechanism  = sentinel;
    ctx.overwrite_query_cb = [](const std::string &) {
      OverwriteDecision d;
      d.action = OverwriteAction::Merge;
      return d;
    };
    InstallStage stage;
    REQUIRE(stage.execute(mod, ctx));

    // Merge never deletes the folder, so this survives by not having been
    // touched. NAMED HONESTLY: this is a pin on "Merge keeps the folder", NOT
    // on "the carry never grew a Merge branch". A carry that wrote the same
    // sentinel a second time on the Merge path would leave this CHECK green -
    // create_empty_file is idempotent, so writing it twice and writing it once
    // are indistinguishable here. That mutation is genuinely unobservable
    // without instrumentation nobody should add for it; the real risk it
    // guards is a Merge that starts deleting, and it does catch that.
    CHECK(fs::exists(mods_dir / "MetaMod" / sentinel));
  }

  SECTION("the headless silent replace carries it too") {
    // No overwrite_query_cb = the CLI path, which removes the folder without
    // asking. It used to carry NOTHING at all - priority, separator and
    // sentinel alike - so the same mod came back enabled, at the bottom of the
    // list, out of its separator, on a headless install.
    TempDir tmp("headless");
    const fs::path mods_dir = tmp.root / "mods";
    fs::create_directories(mods_dir);
    seed_installed(mods_dir, "MetaMod", "MetaMod 1.24.0 old.7z");
    std::ofstream(mods_dir / "MetaMod" / sentinel);

    Mod mod;
    mod.id               = "MetaMod";
    mod.name             = "MetaMod";
    mod.state            = ModState::Extracted;
    mod.archive_filename = "MetaMod 1.25.0 new.7z";
    ModFile staged;
    staged.relative_path = seed_staging(tmp.root, "headless").string();
    mod.files.push_back(staged);

    PipelineContext ctx;
    ctx.mods_dir          = mods_dir.string();
    ctx.disable_mechanism = sentinel;
    InstallStage stage;
    REQUIRE(stage.execute(mod, ctx));

    CHECK(fs::exists(mods_dir / "MetaMod" / sentinel));
    CHECK(ModMeta::load_file(mods_dir / "MetaMod" / "meta.ini").priority() == 17);
  }
}

// The Nexus update verdict is per MOD, not per installed file, so a Replace
// has to carry it too.
//
// write_game_metadata early-returns when meta.ini exists and otherwise creates
// one holding five keys (mod_meta.cpp:804-838). On a Replace that means
// nexus_update / newestversion are destroyed, and the step whose whole job is
// to clear a stale update symbol gates on nexus_update == "true"
// (downloads_controller.cpp). It therefore reads an ABSENT flag, decides
// there is nothing to clear, and the row keeps wearing the symbol for a mod
// that is now current - the user's "still flagged after updating to the newest".
//
// Hermetic: throwaway temp root, real InstallStage, no UI, no network.
TEST_CASE("an overwrite carries the update verdict forward",
          "[engine][install][overwrite][update]") {
  TempDir tmp("verdict");
  const fs::path mods_dir = tmp.root / "mods";
  fs::create_directories(mods_dir);
  seed_installed(mods_dir, "MetaMod", "MetaMod 1.24.0 old.7z");
  {
    auto meta = ModMeta::load_file(mods_dir / "MetaMod" / "meta.ini");
    meta.set("General", "version", "1.24.0");
    // The sweep's own spelling and its own verdict.
    meta.set("General", "nexus_update", "true");
    meta.set("General", "newestversion", "1.25.0");
    meta.set("General", "ignoredVersion", "1.20.0");
    REQUIRE(meta.save_file(mods_dir / "MetaMod" / "meta.ini"));
  }

  Mod mod;
  mod.id               = "MetaMod";
  mod.name             = "MetaMod";
  mod.state            = ModState::Extracted;
  mod.archive_filename = "MetaMod 1.25.0 new.7z";
  mod.version          = "1.25.0";  // the user installed what Nexus offered
  ModFile staged;
  staged.relative_path = seed_staging(tmp.root, "v2 payload").string();
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

  const auto after = ModMeta::load_file(mods_dir / "MetaMod" / "meta.ini");
  INFO("surviving [General]:\n" << after.serialize());
  // The two facts the reconcile compares. With these gone it cannot see a flag
  // to clear; with them present it can, and 1.25.0 == 1.25.0 clears it.
  CHECK(after.get("General", "version") == "1.25.0");
  CHECK(after.get("General", "nexus_update") == "true");
  CHECK(after.get("General", "newestversion") == "1.25.0");
  // The user's own "stop telling me about this one" is per mod, so it rides
  // along too.
  CHECK(engine::Source::Nexus::UpdateCheck::read_ignored_version(after) == "1.20.0");
}

TEST_CASE("a fresh install invents no update verdict",
          "[engine][install][overwrite][update]") {
  // The carry must not write a verdict nobody asked Nexus for - "false" into
  // every new mod would state as fact something no sweep checked, which is the
  // same lie UpdateCheck::run() refuses to state for an unknown mod.
  TempDir tmp("verdict_fresh");
  const fs::path mods_dir = tmp.root / "mods";
  fs::create_directories(mods_dir);
  Mod mod;
  mod.id               = "BrandNew";
  mod.name             = "BrandNew";
  mod.state            = ModState::Extracted;
  mod.archive_filename = "BrandNew-1.25.0.7z";
  mod.version          = "1.25.0";
  ModFile staged;
  staged.relative_path = seed_staging(tmp.root, "brand new").string();
  mod.files.push_back(staged);

  PipelineContext ctx;
  ctx.mods_dir = mods_dir.string();
  InstallStage stage;
  REQUIRE(stage.execute(mod, ctx));

  const auto after = ModMeta::load_file(mods_dir / "BrandNew" / "meta.ini");
  CHECK(after.get("General", "nexus_update").empty());
  CHECK(after.get("General", "newestversion").empty());
  CHECK(engine::Source::Nexus::UpdateCheck::read_ignored_version(after).empty());
}

// A Merge install of an archive that carries NO version must not inherit the
// PREVIOUS install's.
//
// install_stage.cpp's refresh loop copies "version" out of
// ModMeta::from_default, and from_default writes that key only for a non-empty
// version - so a versionless archive never visits the key and the old one
// survives in the meta.ini Merge kept. It is not a stale cache, it is a
// confident wrong answer: the Version column names content that is no longer
// the whole of what is installed. And it is self-sealing, because
// record_installed_version skips the Nexus lookup whenever the value on file
// is non-empty - so the wrong answer could never be corrected by anything, and
// the user saw the old version next to the new files.
//
// Hermetic: throwaway temp root, real InstallStage, no UI, no network.
TEST_CASE("a merge of a versionless archive drops the previous version",
          "[engine][install][overwrite][version]") {
  TempDir tmp("merge_version");
  const fs::path mods_dir = tmp.root / "mods";
  fs::create_directories(mods_dir);
  seed_installed(mods_dir, "MetaMod", "MetaMod 1.24.0 old.7z");
  // The version the previous install recorded. This is the survivor.
  {
    auto meta = ModMeta::load_file(mods_dir / "MetaMod" / "meta.ini");
    meta.set("General", "version", "1.24.0");
    REQUIRE(meta.save_file(mods_dir / "MetaMod" / "meta.ini"));
  }

  Mod mod;
  mod.id               = "MetaMod";
  mod.name             = "MetaMod";
  mod.state            = ModState::Extracted;
  mod.archive_filename = "MetaMod 1.25.0 new.7z";
  // No version: the archive shipped none. This is the shape of the user's own
  // case - a Nexus archive with no meta.ini at all.
  mod.version.clear();
  ModFile staged;
  staged.relative_path = seed_staging(tmp.root, "v2 merge payload").string();
  mod.files.push_back(staged);

  PipelineContext ctx;
  ctx.mods_dir           = mods_dir.string();
  ctx.overwrite_query_cb = [](const std::string &) {
    OverwriteDecision d;
    d.action = OverwriteAction::Merge;
    return d;
  };

  InstallStage stage;
  REQUIRE(stage.execute(mod, ctx));

  const auto after = ModMeta::load_file(mods_dir / "MetaMod" / "meta.ini");
  INFO("surviving [General] version: '" << after.get("General", "version") << "'");
  CHECK(after.get("General", "version").empty());
  // The archive identity still moved, so this is a cleared survivor and not a
  // meta.ini that simply stopped being written.
  CHECK(after.get("General", "installationfile") == "MetaMod 1.25.0 new.7z");
  CHECK(after.priority() == 17);

  // The gate the clearing exists to reopen: record_installed_version skips the
  // Nexus lookup whenever [General] version is non-empty
  // (downloads_controller.cpp:1308), so an empty value is what lets the real
  // version be asked for at all.
  CHECK(after.get("General", "version").empty());

  // A reinstall of the SAME archive keeps its version. The clear is aimed at a
  // survivor, and "survivor" is proven only by the meta naming a DIFFERENT
  // archive - a reinstall of the same file has the same content, so wiping its
  // version would invent a new wrong answer and spend a Nexus request.
  TempDir same("same_archive");
  const fs::path same_mods = same.root / "mods";
  fs::create_directories(same_mods);
  seed_installed(same_mods, "MetaMod", "MetaMod 1.25.0 new.7z");
  {
    auto meta = ModMeta::load_file(same_mods / "MetaMod" / "meta.ini");
    meta.set("General", "version", "1.25.0");
    REQUIRE(meta.save_file(same_mods / "MetaMod" / "meta.ini"));
  }
  Mod again;
  again.id               = "MetaMod";
  again.name             = "MetaMod";
  again.state            = ModState::Extracted;
  again.archive_filename = "MetaMod 1.25.0 new.7z";
  again.version.clear();
  ModFile again_staged;
  again_staged.relative_path = seed_staging(same.root, "same archive again").string();
  again.files.push_back(again_staged);

  PipelineContext same_ctx;
  same_ctx.mods_dir           = same_mods.string();
  same_ctx.overwrite_query_cb = [](const std::string &) {
    OverwriteDecision d;
    d.action = OverwriteAction::Merge;
    return d;
  };
  InstallStage same_stage;
  REQUIRE(same_stage.execute(again, same_ctx));
  CHECK(ModMeta::load_file(same_mods / "MetaMod" / "meta.ini")
            .get("General", "version") == "1.25.0");
}