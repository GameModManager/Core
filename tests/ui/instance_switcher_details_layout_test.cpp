// Offscreen regression test for the instance switcher details pane:
// word-wrapped value labels must never be clipped (height >= heightForWidth)
// at narrow and wide window sizes.
#include "engine/instance/instance.h"
#include "engine/instance/instance_utils.h"
#include "engine/plugin_host/plugin_loader.h"
#include "ui/widgets/instance_switcher_content_widget.h"

#include <QApplication>
#include <QLabel>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {
int test_argc     = 1;
char test_argv0[] = "instance_switcher_details_layout_test";
char *test_argv[] = {test_argv0};
}  // namespace

TEST_CASE("instance switcher details value labels are not clipped",
          "[ui][instance][details]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path root = fs::temp_directory_path() / "gmm_details_layout";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "instances" / "The Binding of Isaac Rebirth", ec);
  std::ofstream(root / "instances" / "The Binding of Isaac Rebirth" / "instance.toml")
      .close();
  engine::set_instances_dir_override(root / "instances");

  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  engine::PluginLoader loader;
  ui::InstanceSwitcherContentWidget w(&loader);
  w.load_instances((root / "instances").string());
  w.show();

  for (int winW : {320, 900}) {
    w.resize(winW, 400);
    QApplication::processEvents();
    // The Location value is a long path that wraps.
    QLabel *loc = nullptr;
    for (auto *l : w.findChildren<QLabel *>()) {
      if (l->wordWrap() && l->text().contains("instances")) {
        loc = l;
        break;
      }
    }
    REQUIRE(loc != nullptr);
    int hfw = loc->heightForWidth(loc->width());
    REQUIRE(loc->height() >= hfw);
  }

  fs::remove_all(root, ec);
}
