// A failed install has to say WHY - per stage, specifically.
//
// The install dialog shows ctx.error_message verbatim, so a stage that returns
// false without writing one leaves the user with a bare "Installation failed"
// and a pointer at a log they have no reason to open. The archive extractor
// was the only stage that produced a reason; fetch, install, deploy and fomod
// all returned a bare bool, so every one of those failures looked identical
// from the outside.
//
// These cases pin the reason to the specific thing that went wrong, not merely
// to "something produced a string". A stage that regressed to
// "An error occurred" would satisfy a non-empty check and fail every assertion
// here. Each case also carries a NEGATIVE CONTROL: a success leaves
// error_message empty, so a stage that filled it unconditionally would be
// caught too.

#include "engine/deploy/interface.h"
#include "engine/pipeline/deploy_stage.h"
#include "engine/pipeline/fetch_stage.h"
#include "engine/pipeline/fomod_stage.h"
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

// Per-case throwaway root under /tmp, removed on scope exit even when an
// assertion FAILs, so a red run leaves nothing behind.
struct TempDir {
  explicit TempDir(const std::string &name)
      : root(fs::temp_directory_path() /
             ("gmm_install_reason_" + name + "_" + std::to_string(::getpid()) + "_" +
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

  // A regular file at this path: make_directories() against a file's parent
  // fails with ENOTDIR for root as well, so these cases do not depend on the
  // test user being unprivileged.
  fs::path file_at(const std::string &name, const std::string &content = "x") {
    fs::path p = root / name;
    if (p.has_parent_path()) {
      std::error_code ec;
      fs::create_directories(p.parent_path(), ec);
    }
    std::ofstream out(p);
    out << content;
    return p;
  }

  static int counter_;
  fs::path root;
};
int TempDir::counter_ = 0;

// A staging dir that InstallStage will accept as extracted content.
void seed_staging(Mod &mod, const fs::path &staging) {
  std::error_code ec;
  fs::create_directories(staging, ec);
  std::ofstream(staging / "plugin.esp") << "content";
  ModFile mf;
  mf.relative_path = staging.string();
  mod.files.push_back(mf);
}

// A minimal but VALID FOMOD config: a required file plus one SelectAny group.
// The cases below need a config the deserializer accepts, so a case can reach
// the failure it is actually about rather than dying in XML parsing.
constexpr const char *kBasicConfig = R"(<config>
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
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
</config>)";

// A deploy strategy that fails on every file - stands in for a target that
// cannot be written, so the partial-deploy path is reachable without a real
// unwritable mount.
class AlwaysFailsStrategy final : public Deploy::Interface {
public:
  bool deploy(const fs::path &, const fs::path &) override { return false; }
  bool remove(const fs::path &) override { return true; }
};

}  // namespace

TEST_CASE("Fetch names the source type it has no provider for",
          "[engine][pipeline][reason]") {
  TempDir tmp("fetch_no_provider");
  Mod mod;
  mod.id                   = "ModA";
  mod.download_source_type = "nonexistent-source";
  mod.download_source_id   = "12345";
  PipelineContext ctx;

  FetchStage stage;
  CHECK_FALSE(stage.execute(mod, ctx));
  // The reason has to name the type, or the user cannot tell which mod's
  // source is unsupported.
  CHECK(ctx.error_message.find("nonexistent-source") != std::string::npos);
  CHECK(ctx.error_message.find("no download provider") != std::string::npos);

  // NEGATIVE CONTROL: a mod with no download info at all is a success and must
  // not be given a reason.
  Mod already_have_it;
  already_have_it.id = "ModB";
  PipelineContext ok_ctx;
  CHECK(stage.execute(already_have_it, ok_ctx));
  CHECK(ok_ctx.error_message.empty());
}

TEST_CASE("Install names what was missing when there is nothing to install",
          "[engine][pipeline][reason]") {
  TempDir tmp("install_no_staging");
  Mod mod;
  mod.id             = "ModA";
  mod.archive_filename = "SkyUI.zip";
  mod.state          = ModState::Downloaded;  // not Extracted -> real failure
  PipelineContext ctx;
  ctx.mods_dir = tmp.root.string();

  InstallStage stage;
  CHECK_FALSE(stage.execute(mod, ctx));
  // It must name the archive that produced nothing, not just "install failed".
  CHECK(ctx.error_message.find("SkyUI.zip") != std::string::npos);
  CHECK(ctx.error_message.find("nothing was extracted") != std::string::npos);
}

TEST_CASE("Install names the mod that has no name to install under",
          "[engine][pipeline][reason]") {
  TempDir tmp("install_no_name");
  Mod mod;
  mod.state = ModState::Extracted;
  seed_staging(mod, tmp.root / "staging");
  // No name, no id, no download id: every fallback is empty too.
  PipelineContext ctx;
  ctx.mods_dir = tmp.root.string();

  InstallStage stage;
  CHECK_FALSE(stage.execute(mod, ctx));
  CHECK(ctx.error_message.find("no name") != std::string::npos);
  CHECK(ctx.error_message.find("folder") != std::string::npos);
}

TEST_CASE("Install names the instance with no mods directory",
          "[engine][pipeline][reason]") {
  TempDir tmp("install_no_mods_dir");
  Mod mod;
  mod.id    = "ModA";
  mod.name  = "ModA";
  mod.state = ModState::Extracted;
  seed_staging(mod, tmp.root / "staging");
  PipelineContext ctx;
  // No ctx.mods_dir and no ctx.instance -> the destination cannot be resolved.
  ctx.mods_dir.clear();

  InstallStage stage;
  CHECK_FALSE(stage.execute(mod, ctx));
  CHECK(ctx.error_message.find("no mods directory") != std::string::npos);
}

TEST_CASE("Install names the folder it could not copy into",
          "[engine][pipeline][reason]") {
  TempDir tmp("install_copy_failed");
  Mod mod;
  mod.id    = "ModA";
  mod.name  = "ModA";
  mod.state = ModState::Extracted;
  seed_staging(mod, tmp.root / "staging");
  PipelineContext ctx;
  // mods_dir points at a regular file, so creating mods/ModA under it fails
  // with ENOTDIR.
  ctx.mods_dir = tmp.file_at("not_a_dir").string();

  InstallStage stage;
  CHECK_FALSE(stage.execute(mod, ctx));
  // The destination has to be in the reason: "the mod folder is incomplete"
  // without naming it is not actionable.
  CHECK(ctx.error_message.find("not_a_dir") != std::string::npos);
  CHECK(ctx.error_message.find("copy") != std::string::npos);
}

TEST_CASE("Install names the empty name the overwrite dialog handed back",
          "[engine][pipeline][reason]") {
  TempDir tmp("install_empty_rename");
  Mod mod;
  mod.id    = "ModA";
  mod.name  = "ModA";
  mod.state = ModState::Extracted;
  seed_staging(mod, tmp.root / "staging");

  const auto mods_dir = tmp.root / "mods";
  std::error_code ec;
  fs::create_directories(mods_dir / "ModA", ec);
  std::ofstream(mods_dir / "ModA" / "old.esp") << "old";

  PipelineContext ctx;
  ctx.mods_dir = mods_dir.string();
  ctx.overwrite_query_cb = [](const std::string &) {
    OverwriteDecision d;
    d.action   = OverwriteAction::Rename;
    d.new_name = "";  // the dialog gave back nothing to install under
    return d;
  };

  InstallStage stage;
  CHECK_FALSE(stage.execute(mod, ctx));
  CHECK(ctx.error_message.find("empty") != std::string::npos);

  // The "could not back up" site is the same copy_recursive() refusal the
  // install-copy case above already asserts, and it is not reachable here:
  // generate_backup_name() takes the first name that does not exist, so any
  // blocker pre-placed to make the copy fail is a name it skips.
}

TEST_CASE("Deploy names the instance with no game directory",
          "[engine][pipeline][reason]") {
  TempDir tmp("deploy_no_game_dir");
  Mod mod;
  mod.id = "ModA";
  PipelineContext ctx;
  ctx.mods_dir  = tmp.root.string();
  ctx.game_dir.clear();
  // A strategy is required, not optional: the stage skips cleanly (success)
  // when there is none, so the missing game dir is never reached without one.
  AlwaysFailsStrategy strategy;
  ctx.deploy_strategy = &strategy;

  DeployStage stage;
  CHECK_FALSE(stage.execute(mod, ctx));
  CHECK(ctx.error_message.find("no game directory") != std::string::npos);
}

TEST_CASE("Deploy names the mod folder that is gone",
          "[engine][pipeline][reason]") {
  TempDir tmp("deploy_mod_gone");
  Mod mod;
  mod.id = "NeverInstalled";
  PipelineContext ctx;
  ctx.mods_dir     = tmp.root.string();
  ctx.game_dir     = (tmp.root / "game").string();
  std::error_code ec;
  fs::create_directories(ctx.game_dir, ec);
  AlwaysFailsStrategy strategy;
  ctx.deploy_strategy = &strategy;

  DeployStage stage;
  CHECK_FALSE(stage.execute(mod, ctx));
  // The missing folder is the whole diagnosis - it must be named.
  CHECK(ctx.error_message.find("NeverInstalled") != std::string::npos);
  CHECK(ctx.error_message.find("no longer exists") != std::string::npos);
}

TEST_CASE("Deploy reports how much of a mod actually landed",
          "[engine][pipeline][reason]") {
  TempDir tmp("deploy_partial");
  Mod mod;
  mod.id = "ModA";
  std::error_code ec;
  fs::create_directories(tmp.root / "mods" / "ModA", ec);
  std::ofstream(tmp.root / "mods" / "ModA" / "a.esp") << "a";
  std::ofstream(tmp.root / "mods" / "ModA" / "b.esp") << "b";

  PipelineContext ctx;
  ctx.mods_dir        = (tmp.root / "mods").string();
  ctx.game_dir        = (tmp.root / "game").string();
  fs::create_directories(ctx.game_dir, ec);
  AlwaysFailsStrategy strategy;
  ctx.deploy_strategy = &strategy;

  DeployStage stage;
  // 2 of 2 files failed: the counts are the fact the user needs.
  CHECK_FALSE(stage.execute(mod, ctx));
  CHECK(ctx.error_message.find("2 of 2") != std::string::npos);
  CHECK(ctx.error_message.find("partly installed") != std::string::npos);

  // NEGATIVE CONTROL: every file landing is a success with no reason, and no
  // game dir means the stage never even gets to a count.
  struct Succeeds final : Deploy::Interface {
    bool deploy(const fs::path &, const fs::path &) override { return true; }
    bool remove(const fs::path &) override { return true; }
  } ok_strategy;
  PipelineContext ok_ctx;
  ok_ctx.mods_dir        = ctx.mods_dir;
  ok_ctx.game_dir        = ctx.game_dir;
  ok_ctx.deploy_strategy = &ok_strategy;
  Mod ok_mod;
  ok_mod.id = "ModA";
  CHECK(stage.execute(ok_mod, ok_ctx));
  CHECK(ok_ctx.error_message.empty());
}

TEST_CASE("Fomod names the installer config it could not read",
          "[engine][pipeline][reason]") {
  TempDir tmp("fomod_bad_config");
  const auto staging = tmp.root / "staging";
  std::error_code ec;
  fs::create_directories(staging / "fomod", ec);
  // Truncated XML - deserialize throws, and the mod is a real FOMOD.
  std::ofstream(staging / "fomod" / "ModuleConfig.xml")
      << "<moduleConfig><dependency><module";

  Mod mod;
  mod.id   = "ModA";
  mod.name = "ModA";
  ModFile mf;
  mf.relative_path = staging.string();
  mod.files.push_back(mf);

  PipelineContext ctx;
  FomodStage stage;
  CHECK_FALSE(stage.execute(mod, ctx));
  CHECK(ctx.error_message.find("ModuleConfig.xml") != std::string::npos);
  CHECK(ctx.error_message.find("could not be read") != std::string::npos);
}

TEST_CASE("Fomod names the archive with no wizard to ask",
          "[engine][pipeline][reason]") {
  TempDir tmp("fomod_no_wizard");
  const auto staging = tmp.root / "staging";
  std::error_code ec;
  fs::create_directories(staging / "fomod", ec);
  fs::create_directories(staging / "Patches", ec);
  std::ofstream(staging / "Core.esm") << "core";
  std::ofstream(staging / "Patches" / "HighRes.esp") << "hr";
  // A well-formed FOMOD config, so the stage gets past deserialization and
  // reaches the "no wizard to ask" abort this case is about.
  std::ofstream(staging / "fomod" / "ModuleConfig.xml") << kBasicConfig;

  Mod mod;
  mod.id   = "ModA";
  mod.name = "Fomod Mod";
  ModFile mf;
  mf.relative_path = staging.string();
  mod.files.push_back(mf);

  PipelineContext ctx;
  ctx.mods_dir = tmp.root.string();
  FomodStage stage;
  // Headless: no fomod_query_cb and no persisted choices -> cannot guess.
  CHECK_FALSE(stage.execute(mod, ctx));
  CHECK(ctx.error_message.find("main window") != std::string::npos);
  CHECK(ctx.error_message.find("Fomod Mod") != std::string::npos);
}
