#include "engine/instance/instance.h"
#include "engine/instance/instance_utils.h"
#include "engine/profile/profile.h"
#include "engine/profile/profile_creation.h"
#include "ui/instance_creation/instance_creation_wizard.h"
#include "ui/settings/settings.h"

#include <QApplication>
#include <QLineEdit>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <set>
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

TEST_CASE("wizard variant combo dedups duplicate variant ids",
          "[instance][wizard][variants]") {
  engine::PluginLoader loader;
  engine::PluginInfo info;
  info.game_id           = "SkyrimSpecialEdition";
  info.game_display_name = "Skyrim Special Edition";
  info.game_support      = true;
  info.variants.push_back({"steam", "Steam"});
  info.variants.push_back({"gog", "GOG"});
  info.variants.push_back({"epic", "Epic Games Store"});
  // duplicate alias registration
  info.variants.push_back({"steam", "Steam"});
  info.variants.push_back({"gog", "GOG"});
  info.variants.push_back({"epic", "Epic Games Store"});
  loader.add_loaded_plugin(info);

  auto v = loader.game_variants("SkyrimSpecialEdition");
  REQUIRE(v.size() == 3);
  // unique by id
  std::set<std::string> ids;
  for (const auto &x : v)
    ids.insert(x.variant_id);
  REQUIRE(ids.size() == v.size());
}

TEST_CASE("wizard name validation messages are specific", "[instance][wizard][name]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path root = fs::temp_directory_path() / "gmm_wiz_name";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "instances", ec);
  engine::set_instances_dir_override(root / "instances");

  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  engine::PluginLoader loader;
  ui::InstanceCreationWizard w(&loader);
  // trigger name page population by navigating? name_edit_ exists after build.
  // set a name with illegal chars
  auto *edit = w.name_edit();
  REQUIRE(edit != nullptr);
  edit->setText("bad:name");
  QString msg = w.name_validation_message();
  REQUIRE(msg.contains("cannot contain"));
  REQUIRE(msg.contains(":"));

  // duplicate: create an instance dir with instance.toml
  fs::create_directories(root / "instances" / "Existing", ec);
  std::ofstream(root / "instances" / "Existing" / "instance.toml").close();
  edit->setText("Existing");
  msg = w.name_validation_message();
  REQUIRE(msg.contains("already exists"));
  REQUIRE(msg.contains("Existing"));

  // valid
  edit->setText("BrandNewName");
  msg = w.name_validation_message();
  REQUIRE(msg.isEmpty());

  fs::remove_all(root, ec);
}
