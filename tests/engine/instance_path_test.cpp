// Engine test for per-folder instance path overrides.
//
// Covers: defaults under <root>/<kind>, override-aware path_for (incl. the
// cache-derived archives/thumbnails), set_path_override clearing back to the
// default, and the instance.toml roundtrip (only non-empty overrides are
// persisted; a read-back instance resolves the same paths).
#include "engine/instance/instance.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

static void require(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::exit(1);
    }
}

int main() {
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

    std::printf("instance_path_test: all checks passed\n");
    return 0;
}
