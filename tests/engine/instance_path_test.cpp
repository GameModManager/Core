// Engine test for per-folder instance path overrides.
//
// Covers: defaults under <root>/<kind>, override-aware path_for (incl. the
// cache-derived archives/thumbnails), set_path_override clearing back to the
// default, and the instance.toml roundtrip (only non-empty overrides are
// persisted; a read-back instance resolves the same paths).
#include "engine/core/instance/instance.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {
void require(bool cond, const char* msg) {
    INFO(msg);
    REQUIRE(cond);
}
}

TEST_CASE("instance path", "[engine]") {
    using engine::Instance;
    using engine::InstanceKind;

    const fs::path root = "/tmp/gmm_instance_path/instances/Test";

    // --- Defaults live under the instance root. ---
    Instance inst = Instance::from_root(root);
    require(inst.path_for(InstanceKind::Mods) == root / "mods",
            "default mods dir is <root>/mods");
    require(inst.path_for(InstanceKind::Downloads) == root / "downloads",
            "default downloads dir is <root>/downloads");
    require(inst.path_for(InstanceKind::Cache) == root / "cache",
            "default cache dir is <root>/cache");
    require(inst.path_for(InstanceKind::Profiles) == root / "profiles",
            "default profiles dir is <root>/profiles");
    require(inst.path_for(InstanceKind::Overwrite) == root / "overwrite",
            "default overwrite dir is <root>/overwrite");
    require(inst.path_for(InstanceKind::Meta) == root / "meta",
            "default meta dir is <root>/meta");

    // --- Overrides replace the defaults. ---
    const fs::path mods = "/data/mods";
    const fs::path dl = "/data/archives";
    const fs::path cache = "/data/cache";
    const fs::path profiles = "/data/profiles";
    const fs::path overwrite = "/data/overwrite";
    inst.set_path_override(InstanceKind::Mods, mods);
    inst.set_path_override(InstanceKind::Downloads, dl);
    inst.set_path_override(InstanceKind::Cache, cache);
    inst.set_path_override(InstanceKind::Profiles, profiles);
    inst.set_path_override(InstanceKind::Overwrite, overwrite);
    require(inst.path_for(InstanceKind::Mods) == mods, "mods override wins");
    require(inst.path_for(InstanceKind::Downloads) == dl, "downloads override wins");
    require(inst.path_for(InstanceKind::Cache) == cache, "cache override wins");
    require(inst.path_for(InstanceKind::Profiles) == profiles, "profiles override wins");
    require(inst.path_for(InstanceKind::Overwrite) == overwrite, "overwrite override wins");
    require(inst.path_for(InstanceKind::Meta) == root / "meta",
            "non-overridable kinds keep the default");
    require(inst.path_override(InstanceKind::Mods) == mods, "path_override returns the value");
    require(inst.path_override(InstanceKind::Meta).empty(),
            "path_override is empty for non-overridable kinds");

    // --- Cache-derived folders follow the cache override. ---
    require(inst.path_for(InstanceKind::CacheArchives) == cache / "archives",
            "archives follow the cache override");
    require(inst.path_for(InstanceKind::CacheThumbnails) == cache / "thumbnails",
            "thumbnails follow the cache override");

    // --- Clearing an override falls back to the default. ---
    inst.set_path_override(InstanceKind::Cache, {});
    require(inst.path_for(InstanceKind::Cache) == root / "cache",
            "cleared cache override falls back to the default");
    require(inst.path_for(InstanceKind::CacheArchives) == root / "cache" / "archives",
            "cleared cache override resets archives default");
    require(inst.path_override(InstanceKind::Cache).empty(), "cleared override reads empty");
    inst.set_path_override(InstanceKind::Cache, cache);

    // --- instance.toml roundtrip. ---
    fs::remove_all(root);
    fs::create_directories(root);
    require(inst.write_toml(), "write_toml succeeds");

    Instance read_back = Instance::from_root(root);
    require(read_back.read_toml(), "read_toml succeeds");
    require(read_back.path_for(InstanceKind::Mods) == mods, "mods override survives toml");
    require(read_back.path_for(InstanceKind::Downloads) == dl, "downloads override survives toml");
    require(read_back.path_for(InstanceKind::Cache) == cache, "cache override survives toml");
    require(read_back.path_for(InstanceKind::Profiles) == profiles, "profiles override survives toml");
    require(read_back.path_for(InstanceKind::Overwrite) == overwrite, "overwrite override survives toml");
    require(read_back.path_for(InstanceKind::Meta) == root / "meta",
            "non-overridden kinds stay default after roundtrip");

    // Only non-empty overrides are written.
    Instance partial = Instance::from_root(root);
    partial.info().game_id = "test_game";
    partial.set_path_override(InstanceKind::Mods, mods);
    require(partial.write_toml(), "partial write_toml succeeds");
    Instance partial_back = Instance::from_root(root);
    require(partial_back.read_toml(), "partial read_toml succeeds");
    require(partial_back.path_for(InstanceKind::Mods) == mods, "mods override roundtrips");
    require(partial_back.path_for(InstanceKind::Downloads) == root / "downloads",
            "unset override stays default after write");
    require(partial_back.info().game_id == "test_game",
            "existing keys preserved alongside new override keys");

    // --- write_key surgical roundtrip (proton_runner). ---
    Instance runner = Instance::from_root(root);
    require(runner.write_key("proton_runner", "Proton 10.0"),
            "write_key sets proton_runner");
    Instance runner_back = Instance::from_root(root);
    require(runner_back.read_toml(), "read_toml after write_key succeeds");
    require(runner_back.info().proton_runner == "Proton 10.0",
            "proton_runner roundtrips through toml");
    require(runner_back.path_for(InstanceKind::Mods) == mods,
            "write_key preserves existing override sections");
    require(runner_back.info().game_id == "test_game",
            "write_key preserves unrelated top-level keys");

    // write_key with an absolute path survives too.
    require(runner.write_key("proton_runner", "/opt/proton/proton"),
            "write_key accepts absolute paths");
    Instance abs_back = Instance::from_root(root);
    require(abs_back.read_toml(), "read_toml after absolute write_key succeeds");
    require(abs_back.info().proton_runner == "/opt/proton/proton",
            "absolute proton_runner roundtrips");

    // Empty value removes the key.
    require(runner.write_key("proton_runner", ""), "write_key with empty value succeeds");
    Instance cleared_back = Instance::from_root(root);
    require(cleared_back.read_toml(), "read_toml after clearing succeeds");
    require(cleared_back.info().proton_runner.empty(),
            "cleared proton_runner reads empty");

    // --- deploy_strategy: write_toml + write_key roundtrips. ---
    Instance strat = Instance::from_root(root);
    strat.info().deploy_strategy = "overlayfs";
    require(strat.write_toml(), "write_toml with deploy_strategy succeeds");
    Instance strat_back = Instance::from_root(root);
    require(strat_back.read_toml(), "read_toml after deploy_strategy write succeeds");
    require(strat_back.info().deploy_strategy == "overlayfs",
            "deploy_strategy roundtrips through write_toml");

    Instance strat_key = Instance::from_root(root);
    require(strat_key.write_key("deploy_strategy", "symlink"),
            "write_key sets deploy_strategy");
    Instance strat_key_back = Instance::from_root(root);
    require(strat_key_back.read_toml(), "read_toml after deploy_strategy write_key succeeds");
    require(strat_key_back.info().deploy_strategy == "symlink",
            "deploy_strategy roundtrips through write_key");
    require(strat_key.write_key("deploy_strategy", ""),
            "write_key with empty value clears deploy_strategy");
    Instance strat_cleared = Instance::from_root(root);
    require(strat_cleared.read_toml(), "read_toml after clearing deploy_strategy succeeds");
    require(strat_cleared.info().deploy_strategy.empty(),
            "cleared deploy_strategy reads empty");

    // --- last_tab roundtrip (Issue #21). ---
    Instance tabbed = Instance::from_root(root);
    tabbed.info().game_id = "test_game";
    tabbed.info().last_tab = "plugins";
    require(tabbed.write_toml(), "write_toml with last_tab succeeds");
    Instance tabbed_back = Instance::from_root(root);
    require(tabbed_back.read_toml(), "read_toml after last_tab write succeeds");
    require(tabbed_back.info().last_tab == "plugins",
            "last_tab roundtrips through toml");
    require(tabbed_back.info().game_id == "test_game",
            "last_tab write preserves unrelated top-level keys");

    // write_key surgical roundtrip for last_tab.
    require(tabbed.write_key("last_tab", "downloads"),
            "write_key sets last_tab");
    Instance tab_key_back = Instance::from_root(root);
    require(tab_key_back.read_toml(), "read_toml after last_tab write_key succeeds");
    require(tab_key_back.info().last_tab == "downloads",
            "last_tab write_key roundtrips through toml");
    require(tab_key_back.info().game_id == "test_game",
            "last_tab write_key preserves unrelated top-level keys");

    // Empty last_tab is not persisted; a fresh instance reads empty (defaults
    // to the first tab).
    require(tabbed.write_key("last_tab", ""), "write_key clears last_tab");
    Instance tab_cleared = Instance::from_root(root);
    require(tab_cleared.read_toml(), "read_toml after clearing last_tab succeeds");
    require(tab_cleared.info().last_tab.empty(),
            "cleared last_tab reads empty");
}
