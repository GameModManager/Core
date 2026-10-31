#include "engine/instance/instance.h"
#include "engine/instance/instance_utils.h"
#include "engine/profile/profile.h"
#include "engine/profile/profile_creation.h"
#include "ui/instance_creation/instance_creation_wizard.h"
#include "ui/settings/settings.h"

#include <QApplication>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

TEST_CASE("instance creation options persist variant and overrides",
          "[instance][wizard]") {
  const fs::path root = fs::temp_directory_path() / "gmm_wiz_opts";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "instances", ec);

  engine::set_instances_dir_override(root / "instances");

  engine::DetectedGame dg;
  dg.game_id     = "generic";
  dg.name        = "Generic";
  dg.steam_appid = 0;

  engine::InstanceCreationOptions opts;
  opts.game_variant = "GOG";
  opts.mods_dir     = root / "custom_mods";

  auto inst =
      engine::create_instance_for_game(dg, root / "instances", "My Instance", opts);
  REQUIRE(!inst.info().root.empty());
  REQUIRE(inst.info().game_variant == "GOG");

  // round-trip via toml
  auto inst2 = engine::Instance::from_root(inst.info().root);
  REQUIRE(inst2.read_toml());
  REQUIRE(inst2.info().game_variant == "GOG");

  // override persisted
  REQUIRE(inst2.path_override(engine::InstanceKind::Mods) == (root / "custom_mods"));

  fs::remove_all(root, ec);
}

TEST_CASE("profile seed writes archive invalidation", "[profile][wizard]") {
  const fs::path root = fs::temp_directory_path() / "gmm_wiz_prof";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  engine::profile::ProfileSeed seed;
  seed.archive_invalidation = true;
  seed.local_settings       = false;
  seed.local_saves          = false;

  auto res = engine::profile::create_fresh_profile(root, "Default", {}, &seed);
  REQUIRE(res.success);

  engine::profile::ProfileManager pm(res.directory);
  REQUIRE(pm.automatic_archive_invalidation() == true);

  fs::remove_all(root, ec);
}
