// P1.4 FOMOD stage-claim plugin test — the host UI bridge MVP (PLAN.md §19.4
// P1.4). Pins:
//   - a test plugin claims the "Fomod" install-template stage via the C ABI
//     (register_stage_claim) for its own game (the .so stem),
//   - main_window's claim_for logic picks the plugin claim over the core
//     FomodStage (game_id match + highest priority),
//   - the plugin's stage handler runs the install through the host UI bridge
//     (GmmHostUi::fomod_wizard): the engine's Qt-free FomodStage does the
//     install work, the wizard is the host's fomod_query_cb (a fake accept
//     here), and the outcome JSON returns to the plugin,
//   - the FOMOD install behaves exactly like the core path: selected files
//     applied, fomod/ pruned, wizard-edited mod name lands on the mod, choices
//     JSON produced for InstallStage (persistence itself is covered by
//     pipeline_test — this test proves only the plugin seam),
//   - wizard cancel surfaces as Canceled (not Failed) through the claimed
//     stage, and the plugin receives the canceled outcome.
//
// Uses the check() PASS/FAIL pattern (Release builds compile out assert()).

#include "engine/fomod/fomod_view_model.h"
#include "engine/instance/instance.h"
#include "engine/model/mod.h"
#include "engine/pipeline/pipeline.h"
#include "engine/pipeline/plugin_claim_stage.h"
#include "engine/plugin_host/plugin_loader.h"
#include "engine/registry/stage_registry.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;
using namespace engine;

#ifndef GMM_FOMOD_PLUGIN_PATH
#define GMM_FOMOD_PLUGIN_PATH "gmm_fomod_stage_plugin.so"
#endif

// The fixture plugin's game_id = its .so stem (plugin_loader.cpp:468).
static const char* const kPluginGame = "gmm_fomod_stage_plugin";


namespace {
void check(bool cond, const std::string& msg) {
    INFO(msg);
    REQUIRE(cond);
}
}

// Build a temp directory tree rooted at a unique path under the system tmp.
// PID + counter keeps paths unique even when an earlier aborted run leaked its
// directory.
struct TempDir {
    fs::path root;
    TempDir() {
        root = fs::temp_directory_path() /
               ("gmm_fomod_stage_plugin_test_" + std::to_string(::getpid()) + "_" +
                std::to_string(counter_++));
        fs::create_directories(root);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(root, ec); }
    static int counter_;
};
int TempDir::counter_ = 0;

const char* kBasicConfig = R"(<config>
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

const char* kHighResChoices =
    R"({"steps":[{"name":"Core","groups":[{"name":"Options","plugins":["High Res"],"deselected":[]}]}]})";

// A real extracted staging dir with a FOMOD installer (same layout as
// pipeline_test's FomodFixture): fomod/ModuleConfig.xml + required Core.esm +
// two SelectAny plugin files.
static void make_fomod_staging(const fs::path& staging) {
    fs::create_directories(staging / "fomod");
    fs::create_directories(staging / "Patches");
    std::ofstream(staging / "Core.esm") << "core";
    std::ofstream(staging / "Patches" / "HighRes.esp") << "hr";
    std::ofstream(staging / "Patches" / "Lite.esp") << "lite";
    std::ofstream(staging / "fomod" / "ModuleConfig.xml") << kBasicConfig;
}

// Replicates main_window's claim_for lambda: best-priority plugin claim for
// (game_id, stage_name).
static std::optional<StageClaim> claim_for(const StageRegistry& reg,
                                           const std::string& game_id,
                                           const std::string& stage_name) {
    std::optional<StageClaim> best;
    for (const auto& c : reg.claims()) {
        if (c.game_id == game_id && c.stage_name == stage_name) {
            if (!best || c.priority > best->priority) best = c;
        }
    }
    return best;
}

static Mod make_mod(const fs::path& staging) {
    Mod mod;
    mod.id = "fomod-mod";
    mod.name = "Fomod Mod";
    mod.state = ModState::Extracted;
    ModFile f;
    f.relative_path = staging.string();
    mod.files.push_back(f);
    return mod;
}

static std::vector<std::string> read_lines(const fs::path& p) {
    std::ifstream in(p);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    return lines;
}

static void test_fomod_claim_via_bridge() {
    std::printf("=== test_fomod_claim_via_bridge ===\n");

    const fs::path log_path = "/tmp/gmm_fomod_stage_plugin_test.log";
    std::error_code ec;
    fs::remove(log_path, ec);
    ::setenv("GMM_TEST_FOMOD_LOG", log_path.c_str(), 1);

    TempDir tmp;
    const fs::path staging = tmp.root / "staging";
    make_fomod_staging(staging);

    // A real instance so the plugin's opaque-handle accessors see a game.
    Instance instance = Instance::portable(tmp.root / "instance");
    instance.info().game_id = kPluginGame;

    PluginLoader loader;
    check(loader.load_plugin(GMM_FOMOD_PLUGIN_PATH),
          "Fomod stage-claim plugin loads");
    check(loader.plugins().size() == 1, "exactly one plugin registered");

    const auto& reg = loader.stage_registry();
    check(reg.get_handler(kPluginGame, "Fomod") != nullptr,
          "StageRegistry has the Fomod claim for the plugin game");
    check(reg.get_handler("nope", "Fomod") == nullptr,
          "no claim leaks into another game");

    // claim_for picks the plugin claim over the core baseline.
    const auto claim = claim_for(reg, kPluginGame, "Fomod");
    check(claim.has_value(), "claim_for finds the plugin claim");
    check(claim.has_value() && claim->priority == 100,
          "claim priority is the registered 100");

    PipelineContext ctx;
    ctx.instance = &instance;
    ctx.game_dir = tmp.root / "game";
    fs::create_directories(ctx.game_dir / "Data");
    ctx.mods_dir = tmp.root / "mods";
    ctx.metadata_file = "meta.ini";
    ctx.deploy_prefix = "Data";

    // The host's wizard seam (ask_fomod in the GUI): a fake accept that also
    // proves the bridge re-entered FomodStage with the right arguments.
    int queried = 0;
    ctx.fomod_query_cb = [&](const std::shared_ptr<FomodViewModel>&,
                             const fs::path& root, const std::string& suggested,
                             const std::string& previous) {
        ++queried;
        check(root == staging, "bridge re-entered FomodStage on the same content root");
        check(suggested == "Fomod Mod", "suggested name reached the wizard");
        check(previous.empty(), "no previous choices for a fresh install");
        FomodDecision d;
        d.accept = true;
        d.choices_json = kHighResChoices;
        d.mod_name = "Bridge Installed Name";
        return d;
    };

    Mod mod = make_mod(staging);

    Pipeline pipeline;
    pipeline.set_context(ctx);  // moves ctx — inspect pipeline.ctx() afterwards
    pipeline.add_stage(std::make_unique<PluginClaimStage>(
        "Fomod", claim->plugin_id, claim->handler));
    const auto result = pipeline.run(mod);
    check(result == PipelineResult::Success,
          "pipeline succeeds through the plugin-claimed stage");
    check(queried == 1, "the wizard was asked exactly once");
    check(mod.name == "Bridge Installed Name",
          "wizard-edited name lands on the mod");
    check(pipeline.ctx().fomod_choices_json == kHighResChoices,
          "choices JSON produced for InstallStage");

    // The engine's FomodStage did the actual install through the bridge.
    check(fs::exists(staging / "Core.esm"), "required file installed");
    check(fs::exists(staging / "Patches" / "HighRes.esp"),
          "selected plugin file installed");
    check(!fs::exists(staging / "Patches" / "Lite.esp"),
          "unselected plugin file pruned");
    check(!fs::exists(staging / "fomod"), "fomod/ pruned from the install");

    // The plugin's side: its log proves the claim won (its handler ran) and it
    // received the install outcome from the host bridge.
    const auto lines = read_lines(log_path);
    check(lines.size() == 3, "plugin log has the three expected lines");
    check(lines.size() == 3 &&
              lines[0].rfind("stage_claimed game=" + std::string(kPluginGame), 0) == 0,
          "plugin handler ran with its own game visible");
    check(lines.size() == 3 &&
              lines[1].rfind("wizard_outcome {\"outcome\":\"installed\"", 0) == 0,
          "plugin received the installed outcome from the host bridge");
    check(lines.size() == 3 &&
              lines[1].find("\"final_name\":\"Bridge Installed Name\"") != std::string::npos,
          "outcome carries the final mod folder name");
    check(lines.size() == 3 &&
              lines[1].find("\"choices\":") != std::string::npos &&
              lines[1].find("\"choices\":{\"steps\":") != std::string::npos,
          "outcome carries the fomod.json choices object");
    check(lines.size() == 3 && lines[2] == "installed_as Bridge Installed Name",
          "plugin parsed the final mod folder name from the outcome");
}

static void test_fomod_cancel_via_bridge() {
    std::printf("=== test_fomod_cancel_via_bridge ===\n");

    const fs::path log_path = "/tmp/gmm_fomod_stage_plugin_test_cancel.log";
    std::error_code ec;
    fs::remove(log_path, ec);
    ::setenv("GMM_TEST_FOMOD_LOG", log_path.c_str(), 1);

    TempDir tmp;
    const fs::path staging = tmp.root / "staging";
    make_fomod_staging(staging);

    Instance instance = Instance::portable(tmp.root / "instance");
    instance.info().game_id = kPluginGame;

    PluginLoader loader;
    check(loader.load_plugin(GMM_FOMOD_PLUGIN_PATH),
          "Fomod stage-claim plugin loads (cancel run)");

    const auto claim = claim_for(loader.stage_registry(), kPluginGame, "Fomod");
    check(claim.has_value(), "claim_for finds the plugin claim (cancel run)");

    PipelineContext ctx;
    ctx.instance = &instance;
    ctx.mods_dir = tmp.root / "mods";
    ctx.fomod_query_cb = [](const std::shared_ptr<FomodViewModel>&, const fs::path&,
                            const std::string&, const std::string&) {
        return FomodDecision{};  // accept == false → the wizard was canceled
    };

    Mod mod = make_mod(staging);

    Pipeline pipeline;
    pipeline.set_context(ctx);  // moves ctx — inspect pipeline.ctx() afterwards
    pipeline.add_stage(std::make_unique<PluginClaimStage>(
        "Fomod", claim->plugin_id, claim->handler));
    const auto result = pipeline.run(mod);
    check(result == PipelineResult::Canceled,
          "wizard cancel surfaces as Canceled, not Failed");
    check(pipeline.ctx().canceled, "ctx.canceled set by the engine stage");

    const auto lines = read_lines(log_path);
    check(lines.size() == 2, "plugin log has stage_claimed + the canceled outcome");
    check(lines.size() == 2 &&
              lines[1].find("\"outcome\":\"canceled\"") != std::string::npos,
          "plugin received the canceled outcome");
    check(lines.size() == 2 &&
              lines[1].find("\"outcome\":\"failed\"") == std::string::npos,
          "canceled is not misreported as failed");
}

TEST_CASE("fomod stage plugin", "[engine]") {
    test_fomod_claim_via_bridge();
    test_fomod_cancel_via_bridge();

}
