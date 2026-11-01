#include "ui/widgets/instance_switcher_content_widget.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

TEST_CASE("instance delete dialog content: no externals", "[ui][instance]") {
  std::vector<fs::path> ext;
  auto c = ui::build_instance_delete_dialog_content(ext);
  REQUIRE_FALSE(c.show_external_list);
  REQUIRE_FALSE(c.has_externals);
  REQUIRE(c.content ==
          QStringLiteral(
              "No external folders - only the instance folder will be deleted."));
  REQUIRE(c.content != QStringLiteral("All checked items will be deleted."));
}

TEST_CASE("instance delete dialog content: with externals", "[ui][instance]") {
  std::vector<fs::path> ext;
  ext.push_back(fs::temp_directory_path() / "ext_mods");
  auto c = ui::build_instance_delete_dialog_content(ext);
  REQUIRE(c.show_external_list);
  REQUIRE(c.has_externals);
  REQUIRE(c.content !=
          QStringLiteral(
              "No external folders - only the instance folder will be deleted."));
  REQUIRE(c.content == QStringLiteral("All checked items will be deleted.\n\nExternal "
                                      "folders (unchecked = left in place):"));
}
