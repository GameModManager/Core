#include "engine/pipeline/pipeline.h"
#include "engine/mod/fomod/fomod_view_model.h"
#include "engine/mod/filetree/staging_layout.h"
#include "engine/game/registry/game_features/game_feature_registry.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/pipeline/fetch_stage.h"
#include "engine/pipeline/extract_stage.h"
#include "engine/pipeline/fomod_stage.h"
#include "engine/pipeline/install_stage.h"
#include "engine/mod/meta/mod_meta.h"
#include "engine/pipeline/stage_stage.h"
#include "engine/pipeline/resolve_stage.h"
#include "engine/pipeline/deploy_stage.h"
#include "engine/pipeline/launch_stage.h"
#include "engine/pipeline/sync_stage.h"
#include "engine/mod/model/mod.h"
#include "engine/source/interface.h"
#include "engine/source/registry.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace engine;

// Build a temp directory tree rooted at a unique path under the system tmp.
// PID + counter keeps paths unique even when an earlier aborted run leaked its
// directory (otherwise a leaked "My Mod 2" from a killed run would contaminate
// the next run's rename-collision expectations).
struct TempDir {
  std::filesystem::path root;
  TempDir() {
    root = std::filesystem::temp_directory_path() /
           ("gmm_pipeline_test_" + std::to_string(::getpid()) + "_" +
            std::to_string(counter_++));
    std::filesystem::create_directories(root);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
  static int counter_;
};
int TempDir::counter_ = 0;

// A staging dir with a real FOMOD: fomod/ModuleConfig.xml plus a required
// Core.esm and two SelectAny plugin files (Patches/HighRes.esp, Patches/Lite.esp).
struct FomodFixture {
  TempDir tmp;
  std::filesystem::path staging;
  explicit FomodFixture(const std::string &config,
                        const std::string &fomodDir = "fomod") {
    staging = tmp.root / "staging";
    std::filesystem::create_directories(staging / fomodDir);
    std::filesystem::create_directories(staging / "Patches");
    std::ofstream(staging / "Core.esm") << "core";
    std::ofstream(staging / "Patches" / "HighRes.esp") << "hr";
    std::ofstream(staging / "Patches" / "Lite.esp") << "lite";
    std::ofstream(staging / fomodDir / "ModuleConfig.xml") << config;
  }
  Mod make_mod(const std::string &name = "Fomod Mod") {
    Mod mod;
    mod.id    = "fomod-mod";
    mod.name  = name;
    mod.state = ModState::Extracted;
    ModFile f;
    f.relative_path = staging.string();
    mod.files.push_back(f);
    return mod;
  }
};

const char *kBasicConfig = R"(<config>
  <moduleName>Test FOMOD</moduleName>
  <requiredInstallFiles>
    <file source="Core.esm" destination=""/>
  </requiredInstallFiles>
  <installSteps>
    <installStep name="Core">
      <optionalFileGroups>
        <group name="Options" type="SelectAny">
          <plugins order="Ascending">
            <plugin name="High Res">
              <files><file source="Patches/HighRes.esp"/></files>
            </plugin>
            <plugin name="Lite">
              <files><file source="Patches/Lite.esp"/></files>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
</config>)";

const char *kHighResChoices =
    R"({"steps":[{"name":"Core","groups":[{"name":"Options","plugins":["High Res"],"deselected":[]}]}]})";

// A helper that returns a FomodDecision accepting with High Res selected.
auto accept_high_res = [](const std::shared_ptr<FomodViewModel> &,
                          const std::filesystem::path &, const std::string &,
                          const std::string &) {
  FomodDecision d;
  d.accept       = true;
  d.choices_json = kHighResChoices;
  return d;
};

// A stage that fails (or cancels) after recording a directory staging entry in
// mod.files. Pipeline::run must remove it so a failed/canceled install never
// leaves the extract <archive>_tmp dir behind.
class StagingFailStage final : public Stage {
public:
  StagingFailStage(std::filesystem::path staging, bool cancel)
      : staging_(std::move(staging)), cancel_(cancel) {}
  bool execute(Mod &mod, PipelineContext &ctx) override {
    ModFile f;
    f.relative_path = staging_.string();
    mod.files.push_back(f);
    if (cancel_)
      ctx.canceled = true;
    return false;
  }
  std::string name() const override { return "Fail"; }

private:
  std::filesystem::path staging_;
  bool cancel_;
};

// A provider that resolves download metadata without touching the network and
// writes a trivial archive in fetch() - lets FetchStage be tested hermetically.
class TestMetaProvider final : public Source::Interface {
public:
  std::string source_type() const override { return "test-meta"; }
  bool fetch(const Mod &, PipelineContext &,
             const std::filesystem::path &dest_path) override {
    std::ofstream(dest_path) << "archive";
    return true;
  }
  Source::SourceDownloadInfo resolve_download_info(const Mod &) const override {
    Source::SourceDownloadInfo info;
    info.archive_name = "Real_Archive-198-489053.7z";
    info.display_name = "Real Mod Name";
    return info;
  }
};

// Same, but resolve_download_info returns nothing (e.g. no API key / probe
// failed) - FetchStage must fall back to the default names and report the
// empty metadata so the UI keeps its placeholder.
class TestBlankProvider final : public Source::Interface {
public:
  std::string source_type() const override { return "test-blank"; }
  bool fetch(const Mod &, PipelineContext &,
             const std::filesystem::path &dest_path) override {
    std::ofstream(dest_path) << "archive";
    return true;
  }
  Source::SourceDownloadInfo resolve_download_info(const Mod &) const override {
    return {};
  }
};
}  // namespace

TEST_CASE("pipeline", "[engine]") {
  {
    Pipeline pipeline;

    PipelineContext ctx;
    pipeline.set_context(ctx);

    pipeline.add_stage(std::make_unique<FetchStage>());
    pipeline.add_stage(std::make_unique<ExtractStage>());
    pipeline.add_stage(std::make_unique<InstallStage>());
    pipeline.add_stage(std::make_unique<StageStage>());
    pipeline.add_stage(std::make_unique<ResolveStage>());
    pipeline.add_stage(std::make_unique<DeployStage>());
    pipeline.add_stage(std::make_unique<LaunchStage>());
    pipeline.add_stage(std::make_unique<SyncStage>());

    Mod mod;
    mod.id      = "test-mod-001";
    mod.name    = "Test Mod";
    mod.version = "1.0";

    REQUIRE(mod.state == ModState::Downloaded);
    REQUIRE(pipeline.run(mod) == engine::PipelineResult::Success);
    REQUIRE(mod.state == ModState::Deployed);
    REQUIRE(mod.id == "test-mod-001");

    std::printf("PASS: pipeline_test — mod flowed through all 8 stages\n");
  }

  // FomodStage: a plain archive (no fomod/ModuleConfig.xml) passes through,
  // and a FOMOD archive is installed via the fomod_query_cb wizard.
  {
    TempDir plain;
    std::filesystem::create_directories(plain.root / "textures");

    FomodStage fomod;
    PipelineContext ctx;
    Mod mod;
    mod.id    = "plain-mod";
    mod.name  = "Plain Mod";
    mod.state = ModState::Extracted;

    ModFile staging;
    staging.relative_path = plain.root.string();
    mod.files.push_back(staging);

    REQUIRE(fomod.execute(mod, ctx));
    std::printf("PASS: pipeline_test — non-FOMOD archive passes FomodStage\n");
  }

  // (a) wizard accepts → FOMOD files installed, fomod/ pruned, choices passed on
  {
    FomodFixture fix(kBasicConfig);
    FomodStage fomod;
    PipelineContext ctx;
    Mod mod = fix.make_mod();
    ctx.fomod_query_cb =
        [&](const std::shared_ptr<FomodViewModel> &, const std::filesystem::path &root,
            const std::string &suggested, const std::string &previous) {
          REQUIRE(root == fix.staging);
          REQUIRE(suggested == "Fomod Mod");
          REQUIRE(previous.empty());
          FomodDecision d;
          d.accept       = true;
          d.choices_json = kHighResChoices;
          d.mod_name     = "Installed Name";
          return d;
        };
    REQUIRE(fomod.execute(mod, ctx));
    REQUIRE(mod.name == "Installed Name");
    REQUIRE(ctx.fomod_choices_json == kHighResChoices);
    REQUIRE(std::filesystem::exists(fix.staging / "Core.esm"));
    REQUIRE(std::filesystem::exists(fix.staging / "Patches" / "HighRes.esp"));
    REQUIRE(!std::filesystem::exists(fix.staging / "Patches" / "Lite.esp"));
    REQUIRE(!std::filesystem::exists(fix.staging / "fomod"));
    std::printf("PASS: pipeline_test — FOMOD wizard accept installs selected files\n");
  }

  // (b) wizard cancel aborts the pipeline
  {
    FomodFixture fix(kBasicConfig);
    FomodStage fomod;
    PipelineContext ctx;
    Mod mod            = fix.make_mod();
    ctx.fomod_query_cb = [](const std::shared_ptr<FomodViewModel> &,
                            const std::filesystem::path &, const std::string &,
                            const std::string &) {
      return FomodDecision{};  // accept == false
    };
    REQUIRE(!fomod.execute(mod, ctx));
    REQUIRE(ctx.fomod_choices_json.empty());
    REQUIRE(ctx.canceled);
    std::printf("PASS: pipeline_test — FOMOD wizard cancel aborts\n");
  }

  // (b3) capitalized "Fomod/" layout (XPMSE-style) is still detected — Windows
  // mod authors ship any casing of the installer dir; detection is
  // case-insensitive like FOMOD Plus's scanner.
  {
    FomodFixture fix(kBasicConfig, "Fomod");
    FomodStage fomod;
    PipelineContext ctx;
    Mod mod            = fix.make_mod();
    int queried        = 0;
    ctx.fomod_query_cb = [&](const std::shared_ptr<FomodViewModel> &,
                             const std::filesystem::path &root, const std::string &,
                             const std::string &) {
      REQUIRE(root == fix.staging);
      ++queried;
      FomodDecision d;
      d.accept       = true;
      d.choices_json = kHighResChoices;
      return d;
    };
    REQUIRE(fomod.execute(mod, ctx));
    REQUIRE(queried == 1);
    REQUIRE(std::filesystem::exists(fix.staging / "Patches" / "HighRes.esp"));
    REQUIRE(!std::filesystem::exists(fix.staging / "Patches" / "Lite.esp"));
    REQUIRE(!std::filesystem::exists(fix.staging / "Fomod"));
    std::printf("PASS: pipeline_test — capitalized Fomod/ layout detected "
                "case-insensitively\n");
  }

  // (b2) wizard Manual → archive contents install as-is (fomod/ pruned)
  {
    FomodFixture fix(kBasicConfig);
    FomodStage fomod;
    PipelineContext ctx;
    Mod mod            = fix.make_mod();
    ctx.fomod_query_cb = [](const std::shared_ptr<FomodViewModel> &,
                            const std::filesystem::path &, const std::string &,
                            const std::string &) {
      FomodDecision d;
      d.manual   = true;
      d.mod_name = "Manual Name";
      return d;
    };
    REQUIRE(fomod.execute(mod, ctx));
    REQUIRE(mod.name == "Manual Name");
    REQUIRE(ctx.fomod_choices_json.empty());
    REQUIRE(std::filesystem::exists(fix.staging / "Core.esm"));
    REQUIRE(std::filesystem::exists(fix.staging / "Patches" / "HighRes.esp"));
    REQUIRE(std::filesystem::exists(fix.staging / "Patches" / "Lite.esp"));
    REQUIRE(!std::filesystem::exists(fix.staging / "fomod"));
    std::printf("PASS: pipeline_test — FOMOD Manual install keeps raw contents\n");
  }

  // (c) <csharpScript> FOMODs are rejected with a clear warning
  {
    FomodFixture fix("<config><moduleName>CS</moduleName><csharpScript>/* unsupported "
                     "*/</csharpScript></config>");
    FomodStage fomod;
    PipelineContext ctx;
    Mod mod = fix.make_mod();
    REQUIRE(!fomod.execute(mod, ctx));
    std::printf("PASS: pipeline_test — C# script FOMOD aborts\n");
  }

  // (d) headless (no callback) + no previous choices → abort, never guess
  {
    FomodFixture fix(kBasicConfig);
    FomodStage fomod;
    PipelineContext ctx;
    Mod mod = fix.make_mod();
    REQUIRE(!fomod.execute(mod, ctx));
    std::printf("PASS: pipeline_test — headless FOMOD without choices aborts\n");
  }

  // (e) headless with previously persisted choices → restored + installed
  {
    FomodFixture fix(kBasicConfig);
    auto mods = fix.tmp.root / "mods";
    std::filesystem::create_directories(mods / "Restored Mod");
    std::ofstream(mods / "Restored Mod" / "meta.ini")
        << "[fomod]\nchoices=" << kHighResChoices << "\n";
    FomodStage fomod;
    PipelineContext ctx;
    ctx.mods_dir = mods;
    Mod mod      = fix.make_mod("Restored Mod");
    REQUIRE(fomod.execute(mod, ctx));
    REQUIRE(ctx.fomod_choices_json == kHighResChoices);
    REQUIRE(std::filesystem::exists(fix.staging / "Patches" / "HighRes.esp"));
    REQUIRE(!std::filesystem::exists(fix.staging / "Patches" / "Lite.esp"));
    std::printf("PASS: pipeline_test — headless FOMOD restores previous choices\n");
  }

  // (f) files missing from the archive abort by default, proceed on request
  {
    const char *kMissingConfig = R"(<config>
  <moduleName>Broken</moduleName>
  <requiredInstallFiles>
    <file source="Core.esm" destination=""/>
    <file source="Missing.txt" destination=""/>
  </requiredInstallFiles>
</config>)";
    {
      FomodFixture fix(kMissingConfig);
      FomodStage fomod;
      PipelineContext ctx;
      Mod mod            = fix.make_mod();
      ctx.fomod_query_cb = [](const std::shared_ptr<FomodViewModel> &,
                              const std::filesystem::path &, const std::string &,
                              const std::string &) {
        FomodDecision d;
        d.accept = true;
        return d;
      };
      REQUIRE(!fomod.execute(mod, ctx));
      std::printf("PASS: pipeline_test — FOMOD missing files abort by default\n");
    }
    {
      FomodFixture fix(kMissingConfig);
      FomodStage fomod;
      PipelineContext ctx;
      Mod mod            = fix.make_mod();
      ctx.fomod_query_cb = [](const std::shared_ptr<FomodViewModel> &,
                              const std::filesystem::path &, const std::string &,
                              const std::string &) {
        FomodDecision d;
        d.accept         = true;
        d.ignore_missing = true;
        return d;
      };
      REQUIRE(fomod.execute(mod, ctx));
      REQUIRE(std::filesystem::exists(fix.staging / "Core.esm"));
      REQUIRE(!std::filesystem::exists(fix.staging / "fomod"));
      std::printf("PASS: pipeline_test — FOMOD missing files ignored on request\n");
    }
  }

  // (g) full flow: FomodStage + InstallStage persist [fomod] choices in the
  // mod folder's meta.ini, and the installed folder contains only the
  // selected files.
  {
    FomodFixture fix(kBasicConfig);
    auto mods = fix.tmp.root / "mods";
    std::filesystem::create_directories(mods);
    FomodStage fomod;
    PipelineContext ctx;
    ctx.mods_dir       = mods;
    Mod mod            = fix.make_mod();
    ctx.fomod_query_cb = accept_high_res;
    REQUIRE(fomod.execute(mod, ctx));
    InstallStage install;
    REQUIRE(install.execute(mod, ctx));
    auto meta_path = mods / "Fomod Mod" / "meta.ini";
    REQUIRE(std::filesystem::exists(meta_path));
    std::ifstream f(meta_path);
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    REQUIRE(content.find("[fomod]") != std::string::npos);
    REQUIRE(content.find(kHighResChoices) != std::string::npos);
    REQUIRE(std::filesystem::exists(mods / "Fomod Mod" / "Core.esm"));
    REQUIRE(std::filesystem::exists(mods / "Fomod Mod" / "Patches" / "HighRes.esp"));
    REQUIRE(!std::filesystem::exists(mods / "Fomod Mod" / "Patches" / "Lite.esp"));
    std::printf(
        "PASS: pipeline_test — FOMOD choices persisted in mod folder meta.ini\n");
  }

  // Regression: the PipelineContext is reused across installs in one session,
  // so per-install state must be reset at the start of every run. A plain
  // (non-FOMOD) mod installed AFTER a genuine FOMOD must NOT inherit the
  // previous mod's [fomod] choices in its meta.ini.
  {
    TempDir env;
    auto mods = env.root / "mods";
    std::filesystem::create_directories(mods);

    auto pipeline = std::make_unique<Pipeline>();
    PipelineContext ctx;
    ctx.mods_dir       = mods;
    ctx.fomod_query_cb = accept_high_res;
    pipeline->set_context(ctx);
    pipeline->add_stage(std::make_unique<FomodStage>());
    pipeline->add_stage(std::make_unique<InstallStage>());

    // Install #1: a genuine FOMOD - sets ctx.fomod_choices_json.
    {
      FomodFixture fix(kBasicConfig);
      Mod mod   = fix.make_mod("Fomod One");
      mod.state = ModState::Extracted;
      REQUIRE(pipeline->run(mod) == PipelineResult::Success);
      REQUIRE(pipeline->ctx().fomod_choices_json == kHighResChoices);
    }

    // Install #2: a plain archive (no fomod dir) through the SAME pipeline.
    // Its meta.ini must not carry the first mod's [fomod] choices.
    {
      TempDir plain;
      std::filesystem::create_directories(plain.root / "textures");
      std::ofstream(plain.root / "textures" / "t.dds") << "t";
      Mod mod;
      mod.id    = "plain-2";
      mod.name  = "Plain Two";
      mod.state = ModState::Extracted;
      ModFile staging;
      staging.relative_path = plain.root.string();
      mod.files.push_back(staging);
      REQUIRE(pipeline->run(mod) == PipelineResult::Success);

      auto meta_path = mods / "Plain Two" / "meta.ini";
      REQUIRE(std::filesystem::exists(meta_path));
      std::ifstream f(meta_path);
      std::string content((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
      REQUIRE(content.find("[fomod]") == std::string::npos);
      REQUIRE(content.find(kHighResChoices) == std::string::npos);
      REQUIRE(!pipeline->ctx().fomod_detected);
      REQUIRE(pipeline->ctx().fomod_choices_json.empty());
    }
    std::printf("PASS: pipeline_test — non-FOMOD install after a FOMOD does not "
                "inherit its choices\n");
  }

  // Workspace-pmrh M3: reinstalling over an existing mod (Merge) refreshes
  // [General] version/installationfile from the install manifest.
  // write_game_metadata early-returns when meta.ini exists (always true on
  // Merge), so without the merge-loop refresh the version column goes
  // stale after an update.
  {
    TempDir env;
    auto mods = env.root / "mods";
    std::filesystem::create_directories(mods);

    // InstallStage removes the staging dir per install - fresh one each.
    auto make_mod = [&](const std::string &tag, const std::string &version,
                        const std::string &archive) {
      auto staging = env.root / ("staging_" + tag);
      std::filesystem::create_directories(staging);
      std::ofstream(staging / "data.txt") << tag;
      Mod mod;
      mod.id               = "versioned-mod";
      mod.name             = "Versioned Mod";
      mod.version          = version;
      mod.archive_filename = archive;
      mod.state            = ModState::Extracted;
      ModFile f;
      f.relative_path = staging.string();
      mod.files.push_back(f);
      return mod;
    };

    InstallStage install;
    PipelineContext ctx;
    ctx.mods_dir = mods;
    auto v1      = make_mod("v1", "1.0", "mod-1.0.zip");
    REQUIRE(install.execute(v1, ctx));
    REQUIRE(ModMeta::load_file(mods / "Versioned Mod" / "meta.ini").version() == "1.0");

    // Reinstall as Merge (keeps load-order position): the version stamp
    // must follow the new manifest.
    PipelineContext ctx2;
    ctx2.mods_dir           = mods;
    ctx2.overwrite_query_cb = [](const std::string &) {
      OverwriteDecision d;
      d.action = OverwriteAction::Merge;
      return d;
    };
    auto v2 = make_mod("v2", "2.0", "mod-2.0.zip");
    REQUIRE(install.execute(v2, ctx2));
    auto merged = ModMeta::load_file(mods / "Versioned Mod" / "meta.ini");
    REQUIRE(merged.version() == "2.0");
    REQUIRE(merged.get("General", "installationfile") == "mod-2.0.zip");
    REQUIRE(merged.get("General", "newestVersion") == "2.0");
    REQUIRE(merged.get("General", "installationFile") == "mod-2.0.zip");
    std::printf("PASS: pipeline_test — reinstall (Merge) refreshes version from the "
                "manifest\n");
  }

  // InstallStage overwrite query flow: when the target mod folder already
  // exists, overwrite_query_cb decides Merge/Replace/Rename/Cancel instead
  // of the silent-replace default.
  {
    TempDir env;
    auto staging = env.root / "staging";
    auto mods    = env.root / "mods";
    std::filesystem::create_directories(mods);

    // InstallStage deletes the staging dir after each install, so every
    // test recreates it.
    auto reset_staging = [&] {
      std::error_code ec;
      std::filesystem::remove_all(staging, ec);
      std::filesystem::create_directories(staging);
      std::ofstream(staging / "b.txt") << "b";
    };

    auto make_mod = [&](const std::filesystem::path &src) {
      Mod mod;
      mod.id      = "qmod";
      mod.name    = "My Mod";
      mod.version = "1.0";
      mod.state   = ModState::Extracted;
      ModFile f;
      f.relative_path = src.string();
      mod.files.push_back(f);
      return mod;
    };
    auto install = [&](Mod &mod, PipelineContext &ctx) {
      InstallStage stage;
      return stage.execute(mod, ctx);
    };

    // (a) no callback -> silent replace
    {
      reset_staging();
      auto dir = mods / "My Mod";
      std::filesystem::create_directories(dir);
      std::ofstream(dir / "old.txt") << "old";
      PipelineContext ctx;
      ctx.mods_dir = mods;
      Mod mod      = make_mod(staging);
      REQUIRE(install(mod, ctx));
      REQUIRE(mod.state == ModState::Installed);
      REQUIRE(!std::filesystem::exists(dir / "old.txt"));
      REQUIRE(std::filesystem::exists(dir / "b.txt"));
      REQUIRE(ctx.installed_mod_folder == "My Mod");
      std::printf("PASS: install overwrite — headless silent replace\n");
    }

    // (b) Merge keeps existing files and adds the new ones
    {
      reset_staging();
      auto dir = mods / "My Mod";
      std::filesystem::create_directories(dir);
      std::ofstream(dir / "old.txt") << "old";
      PipelineContext ctx;
      ctx.mods_dir           = mods;
      ctx.overwrite_query_cb = [](const std::string &) {
        return OverwriteDecision{OverwriteAction::Merge};
      };
      Mod mod = make_mod(staging);
      REQUIRE(install(mod, ctx));
      REQUIRE(mod.state == ModState::Installed);
      REQUIRE(std::filesystem::exists(dir / "old.txt"));
      REQUIRE(std::filesystem::exists(dir / "b.txt"));
      std::printf("PASS: install overwrite — merge keeps existing files\n");
    }

    // (c) Replace deletes the old folder before installing
    {
      reset_staging();
      auto dir = mods / "My Mod";
      std::filesystem::create_directories(dir);
      std::ofstream(dir / "old.txt") << "old";
      PipelineContext ctx;
      ctx.mods_dir           = mods;
      ctx.overwrite_query_cb = [](const std::string &) {
        return OverwriteDecision{OverwriteAction::Replace};
      };
      Mod mod = make_mod(staging);
      REQUIRE(install(mod, ctx));
      REQUIRE(mod.state == ModState::Installed);
      REQUIRE(!std::filesystem::exists(dir / "old.txt"));
      REQUIRE(std::filesystem::exists(dir / "b.txt"));
      std::printf("PASS: install overwrite — replace deletes old files\n");
    }

    // (d) Replace + backup copies the old folder to <name>_backup first
    {
      reset_staging();
      auto dir = mods / "My Mod";
      std::filesystem::create_directories(dir);
      std::ofstream(dir / "old.txt") << "old";
      PipelineContext ctx;
      ctx.mods_dir           = mods;
      ctx.overwrite_query_cb = [](const std::string &) {
        return OverwriteDecision{OverwriteAction::Replace, /*backup=*/true};
      };
      Mod mod = make_mod(staging);
      REQUIRE(install(mod, ctx));
      auto backup = mods / "My Mod_backup";
      REQUIRE(std::filesystem::exists(backup / "old.txt"));
      REQUIRE(!std::filesystem::exists(dir / "old.txt"));
      std::printf("PASS: install overwrite — replace keeps a backup\n");
    }

    // (e) Rename installs under the new folder and leaves the old one alone
    {
      reset_staging();
      auto dir = mods / "My Mod";
      std::filesystem::create_directories(dir);
      std::ofstream(dir / "old.txt") << "old";
      PipelineContext ctx;
      ctx.mods_dir           = mods;
      ctx.overwrite_query_cb = [](const std::string &) {
        OverwriteDecision d;
        d.action   = OverwriteAction::Rename;
        d.new_name = "My Mod 2";
        return d;
      };
      Mod mod = make_mod(staging);
      REQUIRE(install(mod, ctx));
      REQUIRE(mod.state == ModState::Installed);
      REQUIRE(mod.id == "My Mod 2");
      REQUIRE(std::filesystem::exists(dir / "old.txt"));
      REQUIRE(std::filesystem::exists(mods / "My Mod 2" / "b.txt"));
      REQUIRE(ctx.installed_mod_folder == "My Mod 2");
      std::printf("PASS: install overwrite — rename installs separately\n");
    }

    // (f) Rename re-checks: a new name that also exists re-asks (loop)
    {
      reset_staging();
      auto dir  = mods / "My Mod";
      auto dir2 = mods / "My Mod 2";
      std::filesystem::create_directories(dir);
      std::ofstream(dir / "old.txt") << "old";
      std::filesystem::create_directories(dir2);
      std::ofstream(dir2 / "old.txt") << "old";
      int calls = 0;
      PipelineContext ctx;
      ctx.mods_dir           = mods;
      ctx.overwrite_query_cb = [&](const std::string &) {
        ++calls;
        OverwriteDecision d;
        d.action   = OverwriteAction::Rename;
        d.new_name = calls == 1 ? "My Mod 2" : "My Mod 3";
        return d;
      };
      Mod mod = make_mod(staging);
      REQUIRE(install(mod, ctx));
      REQUIRE(mod.state == ModState::Installed);
      REQUIRE(calls == 2);
      REQUIRE(mod.id == "My Mod 3");
      REQUIRE(std::filesystem::exists(mods / "My Mod 3" / "b.txt"));
      REQUIRE(ctx.installed_mod_folder == "My Mod 3");
      std::printf("PASS: install overwrite — rename loop re-checks collisions\n");
    }

    // (g) Cancel aborts the install, leaving the existing folder untouched
    {
      reset_staging();
      auto dir = mods / "My Mod";
      std::error_code ec;
      std::filesystem::remove_all(dir, ec);  // drop leftovers from earlier tests
      std::filesystem::create_directories(dir);
      std::ofstream(dir / "old.txt") << "old";
      PipelineContext ctx;
      ctx.mods_dir           = mods;
      ctx.overwrite_query_cb = [](const std::string &) {
        return OverwriteDecision{OverwriteAction::Cancel};
      };
      Mod mod = make_mod(staging);
      REQUIRE(!install(mod, ctx));
      REQUIRE(mod.state != ModState::Installed);
      REQUIRE(ctx.canceled);
      REQUIRE(std::filesystem::exists(dir / "old.txt"));
      REQUIRE(!std::filesystem::exists(dir / "b.txt"));
      std::printf("PASS: install overwrite — cancel aborts cleanly\n");
    }

    // (h) Replace reports the archive the deleted mod came from, read off its
    // meta.ini before the delete takes it away. That is the whole of
    // MO2's modReplaced payload (installationmanager.cpp:418-424) and the only
    // way the UI can find the download row to un-claim.
    {
      reset_staging();
      auto dir = mods / "My Mod";
      std::error_code ec;
      std::filesystem::remove_all(dir, ec);
      std::filesystem::create_directories(dir);
      std::ofstream(dir / "old.txt") << "old";
      ModMeta::from_default("My Mod", "nexus", "1234", "old-archive.zip", "1.0")
          .save_file(dir / "meta.ini");
      PipelineContext ctx;
      ctx.mods_dir           = mods;
      ctx.overwrite_query_cb = [](const std::string &) {
        return OverwriteDecision{OverwriteAction::Replace};
      };
      Mod mod = make_mod(staging);
      mod.archive_filename = "new-archive.zip";
      REQUIRE(install(mod, ctx));
      REQUIRE(ctx.replaced_archive == "old-archive.zip");
      // The new install's own meta.ini names the new archive, not the old one.
      const auto after = ModMeta::load_file(dir / "meta.ini");
      REQUIRE(after.get("General", "installationfile") == "new-archive.zip");
      std::printf("PASS: install overwrite — replace reports the old archive\n");
    }

    // (i) Only Replace reports anything. A folder with a meta.ini that Merge,
    // Rename or Cancel leaves in place must not claim its mod was deleted.
    {
      auto leaves_alone = [&](OverwriteAction action, const char *what) {
        reset_staging();
        auto dir = mods / "My Mod";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "old.txt") << "old";
        ModMeta::from_default("My Mod", "nexus", "1234", "old-archive.zip", "1.0")
            .save_file(dir / "meta.ini");
        PipelineContext ctx;
        ctx.mods_dir = mods;
        ctx.overwrite_query_cb = [&](const std::string &) {
          OverwriteDecision d;
          d.action = action;
          if (action == OverwriteAction::Cancel)
            return d;
          if (action == OverwriteAction::Rename)
            d.new_name = "My Mod 9";
          return d;
        };
        Mod mod = make_mod(staging);
        const bool ok = install(mod, ctx);
        CAPTURE(what);
        REQUIRE(ctx.replaced_archive.empty());
        return ok;
      };
      REQUIRE(leaves_alone(OverwriteAction::Merge, "merge"));
      REQUIRE(leaves_alone(OverwriteAction::Rename, "rename"));
      REQUIRE_FALSE(leaves_alone(OverwriteAction::Cancel, "cancel"));
      std::printf("PASS: install overwrite — only Replace reports the old archive\n");
    }
  }

  // normalize_staging_root (MO2 InstallerQuick::getSimpleArchiveBase +
  // GamebryoModDataChecker): peel only wrappers the data-dir check says are
  // safe, merge a lone "Data"+readme top layer up into the root, and never
  // touch FOMOD archives.
  {
    // (a) SKSE/Plugins already looks like the game's data dir - no peel
    {
      TempDir env;
      auto root = env.root / "staging";
      std::filesystem::create_directories(root / "SKSE" / "Plugins");
      std::ofstream(root / "SKSE" / "Plugins" / "x.dll") << "x";
      auto r = normalize_staging_root(root, "Data");
      REQUIRE((r.simple && !r.fomod && !r.merged_data_dir));
      REQUIRE(r.peeled_folder_hint.empty());
      REQUIRE(std::filesystem::exists(root / "SKSE" / "Plugins" / "x.dll"));
      std::printf("PASS: normalize keeps a real data folder (SKSE) in place\n");
    }

    // (b) Data/SKSE/Plugins: a lone Data wrapper is peeled
    {
      TempDir env;
      auto root = env.root / "staging";
      std::filesystem::create_directories(root / "Data" / "SKSE" / "Plugins");
      std::ofstream(root / "Data" / "SKSE" / "Plugins" / "x.dll") << "x";
      auto r = normalize_staging_root(root, "Data");
      REQUIRE((r.simple && r.peeled_folder_hint == "Data"));
      REQUIRE(std::filesystem::exists(root / "SKSE" / "Plugins" / "x.dll"));
      REQUIRE(!std::filesystem::exists(root / "Data"));
      std::printf("PASS: normalize unwraps a lone Data folder\n");
    }

    // (c) Wrapper/SKSE/Plugins: a non-data wrapper is peeled and reported
    {
      TempDir env;
      auto root = env.root / "staging";
      std::filesystem::create_directories(root / "Wrapper" / "SKSE" / "Plugins");
      std::ofstream(root / "Wrapper" / "SKSE" / "Plugins" / "x.dll") << "x";
      auto r = normalize_staging_root(root, "Data");
      REQUIRE((r.simple && r.peeled_folder_hint == "Wrapper"));
      REQUIRE(std::filesystem::exists(root / "SKSE" / "Plugins" / "x.dll"));
      REQUIRE(!std::filesystem::exists(root / "Wrapper"));
      std::printf(
          "PASS: normalize peels a non-data wrapper and reports the name hint\n");
    }

    // (d) lone Data + readme top layer is merged up (isDataTextArchiveTopLayer)
    {
      TempDir env;
      auto root = env.root / "staging";
      std::filesystem::create_directories(root / "Data" / "SKSE" / "Plugins");
      std::ofstream(root / "Data" / "SKSE" / "Plugins" / "x.dll") << "x";
      std::ofstream(root / "readme.txt") << "readme";
      auto r = normalize_staging_root(root, "Data");
      REQUIRE((r.simple && r.merged_data_dir && r.peeled_folder_hint.empty()));
      REQUIRE(std::filesystem::exists(root / "SKSE" / "Plugins" / "x.dll"));
      REQUIRE(std::filesystem::exists(root / "readme.txt"));
      REQUIRE(!std::filesystem::exists(root / "Data"));
      std::printf(
          "PASS: normalize merges a lone Data+readme top layer into the root\n");
    }

    // (e) FOMOD archives are never reshaped
    {
      TempDir env;
      auto root = env.root / "staging";
      std::filesystem::create_directories(root / "fomod");
      std::filesystem::create_directories(root / "meshes");
      std::ofstream(root / "fomod" / "ModuleConfig.xml") << "<config/>";
      std::ofstream(root / "fomod" / "Info.xml") << "<info/>";
      std::ofstream(root / "meshes" / "m.nif") << "x";
      auto r = normalize_staging_root(root, "Data");
      REQUIRE((r.fomod && !r.simple && !r.merged_data_dir));
      REQUIRE(std::filesystem::exists(root / "fomod" / "ModuleConfig.xml"));
      REQUIRE(std::filesystem::exists(root / "meshes" / "m.nif"));
      std::printf("PASS: normalize leaves FOMOD archives untouched\n");
    }
  }

  // A wrapper-less archive whose whole content is one top-level resources/
  // folder (Isaac) must keep that folder: it IS the game's data dir, so there
  // is no wrapper, the archive stem names the mod, and nothing moves - the mod
  // lands in mods/<stem>/resources/, not mods/resources/. Whether the top level
  // is data or a wrapper is the game's to answer, and the answer is read from
  // the same data_checker_for() the install context is built from. "resources"
  // is deliberately absent from the engine's Bethesda fallback set, so the last
  // block below is what pins that the DECLARATION decides this: drop the
  // checker from PipelineContext and the first assertion goes red.
  {
    engine::Game::Features::Registry::instance().clear();
    engine::GameKnowledge knowledge;
    knowledge.set("isaac", "mod_valid_dirs", "resources,resources-dlc3");
    knowledge.set("isaac", "mod_valid_exts", "");
    const auto isaac = engine::data_checker_for(knowledge, "isaac");
    REQUIRE(isaac != nullptr);

    // No wrapper. The predicate ExtractStage reads - simple with an empty peel
    // chain - holds, so the stem names the mod and the tree is left alone.
    TempDir flat;
    const auto flat_root     = flat.root / "staging";
    const auto flat_greeting = flat_root / "resources" / "greetings" / "isaac.png";
    std::filesystem::create_directories(flat_greeting.parent_path());
    std::ofstream(flat_greeting) << "x";
    const auto flat_verdict = analyze_staging_root(flat_root, "Data", isaac);
    REQUIRE(flat_verdict.simple);
    REQUIRE(flat_verdict.peel_chain.empty());
    REQUIRE(std::filesystem::exists(flat_greeting));

    // A genuine MyMod/ wrapper is still a wrapper: the root holds a folder the
    // game never declared, so the chain is not empty, the predicate does not
    // hold, and the mod is named MyMod rather than the archive stem.
    TempDir wrapped;
    const auto wrapped_root = wrapped.root / "staging";
    const auto wrapped_greeting =
        wrapped_root / "MyMod" / "resources" / "greetings" / "isaac.png";
    std::filesystem::create_directories(wrapped_greeting.parent_path());
    std::ofstream(wrapped_greeting) << "x";
    const auto wrapped_verdict = analyze_staging_root(wrapped_root, "Data", isaac);
    REQUIRE(wrapped_verdict.simple);
    REQUIRE(wrapped_verdict.peel_chain == std::vector<std::string>{"MyMod"});
    REQUIRE(wrapped_verdict.peeled_folder_hint == "MyMod");

    // Negative control: the identical wrapper-less tree with nothing declared
    // is NOT game data, which is the regression the plumbing exists to stop -
    // unthread the checker and this is what the first block starts failing on.
    TempDir undeclared;
    const auto bare_root     = undeclared.root / "staging";
    const auto bare_greeting = bare_root / "resources" / "greetings" / "isaac.png";
    std::filesystem::create_directories(bare_greeting.parent_path());
    std::ofstream(bare_greeting) << "x";
    const auto bare_verdict = analyze_staging_root(bare_root, "Data", nullptr);
    REQUIRE_FALSE((bare_verdict.simple && bare_verdict.peel_chain.empty()));
    std::printf(
        "PASS: extract layout - a wrapper-less resources/ archive keeps its stem, "
        "a MyMod/ wrapper still names the mod\n");
  }

  // InstallStage name dialog (MO2 SimpleInstallDialog): cancel aborts the
  // install cleanly, accept installs under the chosen name, and FOMOD
  // installs (their wizard owns the name) never trigger it.
  {
    TempDir env;
    auto staging = env.root / "staging";
    auto mods    = env.root / "mods";
    std::filesystem::create_directories(staging);
    std::filesystem::create_directories(mods);
    std::ofstream(staging / "a.txt") << "a";
    auto make_mod = [&] {
      Mod mod;
      mod.id               = "nmod";
      mod.name             = "Name Mod";
      mod.archive_filename = "NameMod.zip";
      mod.state            = ModState::Extracted;
      ModFile f;
      f.relative_path = staging.string();
      mod.files.push_back(f);
      return mod;
    };

    // (a) cancel (nullopt) aborts, leaves everything untouched
    {
      PipelineContext ctx;
      ctx.mods_dir      = mods;
      ctx.name_query_cb = [](const std::string &, const std::string &) {
        return std::optional<std::string>();  // nullopt = cancel
      };
      Mod mod = make_mod();
      InstallStage stage;
      REQUIRE(!stage.execute(mod, ctx));
      REQUIRE(ctx.canceled);
      REQUIRE(mod.state != ModState::Installed);
      REQUIRE(!std::filesystem::exists(mods / "Name Mod"));
      std::printf("PASS: install name dialog cancel aborts cleanly\n");
    }

    // (b) accept installs under the chosen name (caller adds only that folder)
    {
      PipelineContext ctx;
      ctx.mods_dir      = mods;
      ctx.name_query_cb = [](const std::string &suggested, const std::string &archive) {
        REQUIRE(suggested == "Name Mod");
        REQUIRE(archive == "NameMod.zip");
        return std::optional<std::string>("Chosen Name");
      };
      Mod mod = make_mod();
      InstallStage stage;
      REQUIRE(stage.execute(mod, ctx));
      REQUIRE(mod.name == "Chosen Name");
      REQUIRE(ctx.installed_mod_folder == "Chosen Name");
      REQUIRE(std::filesystem::exists(mods / "Chosen Name" / "a.txt"));
      REQUIRE(!std::filesystem::exists(mods / "Name Mod"));
      std::printf("PASS: install name dialog accept installs under the chosen name\n");
    }

    // (c) FOMOD installs skip the name dialog entirely
    {
      // The install in (b) cleaned up the staging dir - recreate it.
      std::filesystem::create_directories(staging);
      std::ofstream(staging / "a.txt") << "a";
      PipelineContext ctx;
      ctx.mods_dir       = mods;
      ctx.fomod_detected = true;
      bool called        = false;
      ctx.name_query_cb  = [&](const std::string &, const std::string &) {
        called = true;
        return std::optional<std::string>("Wrong Name");
      };
      Mod mod = make_mod();
      InstallStage stage;
      REQUIRE(stage.execute(mod, ctx));
      REQUIRE(!called);
      REQUIRE(mod.name == "Name Mod");
      REQUIRE(ctx.installed_mod_folder == "Name Mod");
      REQUIRE(std::filesystem::exists(mods / "Name Mod" / "a.txt"));
      std::printf("PASS: install name dialog skipped for FOMOD installs\n");
    }
  }

  // Pipeline::run: a canceled or failed stage reports Canceled vs Failed
  // distinctly and cleans up the extract staging dir (<archive>_tmp leak).
  for (bool cancel : {false, true}) {
    TempDir tmp;
    auto staging = tmp.root / "arch_tmp";
    std::filesystem::create_directories(staging);
    std::ofstream(staging / "f.txt") << "x";

    Pipeline pipeline;
    PipelineContext ctx;
    pipeline.set_context(ctx);
    pipeline.add_stage(std::make_unique<StagingFailStage>(staging, cancel));

    Mod mod;
    mod.id      = "cleanup";
    mod.state   = ModState::Downloaded;
    auto result = pipeline.run(mod);
    REQUIRE(result == (cancel ? PipelineResult::Canceled : PipelineResult::Failed));
    REQUIRE(!std::filesystem::exists(staging));
    std::printf("PASS: pipeline_test — run %s cleans up staging dir\n",
                cancel ? "cancel" : "failure");
  }

  // FetchStage on_download_meta: as soon as the provider resolves the
  // download metadata (before any bytes flow), the callback carries the real
  // archive name + display name so the UI can replace its placeholder row
  // name immediately instead of waiting for download_complete.
  {
    engine::Source::Registry::instance().register_provider(
        std::make_unique<TestMetaProvider>());
    TempDir tmp;
    auto mods = tmp.root / "mods";
    std::filesystem::create_directories(mods);
    FetchStage stage;
    PipelineContext ctx;
    ctx.mods_dir         = mods;
    bool meta_fired      = false;
    ctx.on_download_meta = [&](const std::string &archive_name,
                               const std::string &display_name) {
      meta_fired = true;
      REQUIRE(archive_name == "Real_Archive-198-489053.7z");
      REQUIRE(display_name == "Real Mod Name");
    };
    Mod mod;
    mod.id                   = "198-489053";
    mod.name                 = "Mod file 198-489053";  // the worker's placeholder
    mod.download_source_type = "test-meta";
    mod.download_source_id   = "198";
    mod.download_nxm.file_id = 489053;
    REQUIRE(stage.execute(mod, ctx));
    REQUIRE(meta_fired);
    REQUIRE(mod.name == "Real Mod Name");
    REQUIRE(mod.archive_filename == "Real_Archive-198-489053.7z");
    REQUIRE(
        std::filesystem::exists(tmp.root / "downloads" / "Real_Archive-198-489053.7z"));
    std::printf("PASS: FetchStage on_download_meta carries the resolved name before "
                "bytes flow\n");
  }

  // Same callback when the provider has no metadata: fires empty (so the UI
  // keeps its placeholder), mod.name is untouched, and the default
  // "<source_id>.zip" archive name is used.
  {
    engine::Source::Registry::instance().register_provider(
        std::make_unique<TestBlankProvider>());
    TempDir tmp;
    auto mods = tmp.root / "mods";
    std::filesystem::create_directories(mods);
    FetchStage stage;
    PipelineContext ctx;
    ctx.mods_dir = mods;
    std::string got_archive, got_display;
    ctx.on_download_meta = [&](const std::string &a, const std::string &d) {
      got_archive = a;
      got_display = d;
    };
    Mod mod;
    mod.id                   = "blank";
    mod.name                 = "Mod file blank";  // placeholder
    mod.download_source_type = "test-blank";
    mod.download_source_id   = "7";
    REQUIRE(stage.execute(mod, ctx));
    REQUIRE((got_archive.empty() && got_display.empty()));
    REQUIRE(mod.name == "Mod file blank");
    REQUIRE(mod.archive_filename == "7.zip");
    REQUIRE(std::filesystem::exists(tmp.root / "downloads" / "7.zip"));
    std::printf("PASS: FetchStage on_download_meta fires empty when the provider has "
                "no info\n");
  }

  // A mod with no registered source but an already-resolved download URL is the
  // transport case: modl:// is a URI handler, so a link from a host that names
  // no source leaves the mod "manual" and the bytes still come down. Without a
  // URL there is nothing to fetch, and the honest answer is no provider.
  {
    Mod mod;
    mod.download_source_type = "manual";
    REQUIRE(fetch_provider_for(mod) == nullptr);

    mod.download_url = "https://example.com/path/Mod.7z";
    auto *transport  = fetch_provider_for(mod);
    REQUIRE(transport != nullptr);
    REQUIRE(transport->display_name() == "Direct URL");

    // The upgrade path: an older build stamped "direct" on every modl://
    // download. A meta the scan has not rewritten yet still resolves to the
    // transport rather than failing the install.
    Mod legacy                  = mod;
    legacy.download_source_type = "direct";
    REQUIRE(fetch_provider_for(legacy) != nullptr);

    // A real source still wins over the transport.
    Mod registered                  = mod;
    registered.download_source_type = "test-blank";
    REQUIRE(fetch_provider_for(registered) != transport);
    std::printf("PASS: fetch_provider_for routes a resolved direct URL to the "
                "transport\n");
  }
}
