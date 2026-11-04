// Regression test for Workspace-2sy4: resolve_proton_prefix must find a
// compatdata prefix on a secondary Steam library, not just the default root.

#include "platform/linux/linux_platform.h"

#include <cstdlib>
#include <filesystem>
#include <unistd.h>
#include <fstream>
#include <string>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {
fs::path make_layout(bool secondary_has_compat, bool default_has_compat) {
  const fs::path root =
      fs::temp_directory_path() / ("gmm_proton_prefix_" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);

  const fs::path default_root = root / "home" / ".local" / "share" / "Steam";
  const fs::path secondary    = root / "secondary";
  fs::create_directories(default_root / "steamapps");
  fs::create_directories(secondary / "steamapps");

  // libraryfolders.vdf under the default root lists both libraries.
  {
    std::ofstream f(default_root / "steamapps" / "libraryfolders.vdf");
    f << "\"libraryfolders\"\n{\n";
    f << "  \"0\"\n  {\n    \"path\"    \"" << default_root.string() << "\"\n  }\n";
    f << "  \"1\"\n  {\n    \"path\"    \"" << secondary.string() << "\"\n  }\n";
    f << "}\n";
  }

  if (default_has_compat) {
    fs::create_directories(default_root / "steamapps" / "compatdata" / "489830");
  }
  if (secondary_has_compat) {
    fs::create_directories(secondary / "steamapps" / "compatdata" / "377160");
  }
  // appmanifest for FO4 on secondary
  {
    std::ofstream f(secondary / "steamapps" / "appmanifest_377160.acf");
    f << "\"AppState\" { \"appid\" \"377160\" }\n";
  }
  // appmanifest for SkyrimSE on default
  {
    std::ofstream f(default_root / "steamapps" / "appmanifest_489830.acf");
    f << "\"AppState\" { \"appid\" \"489830\" }\n";
  }

  ::setenv("HOME", (root / "home").c_str(), 1);
  return root;
}
}  // namespace

TEST_CASE("proton prefix resolves on secondary library", "[engine]") {
  make_layout(true, true);
  auto p = engine::LinuxPlatform().resolve_proton_prefix(377160);
  REQUIRE(p == (fs::temp_directory_path() /
                ("gmm_proton_prefix_" + std::to_string(::getpid()))) /
                   "secondary" / "steamapps" / "compatdata" / "377160");
  // default-only game still resolves to default root
  auto p2 = engine::LinuxPlatform().resolve_proton_prefix(489830);
  REQUIRE(p2 == (fs::temp_directory_path() /
                 ("gmm_proton_prefix_" + std::to_string(::getpid()))) /
                    "home" / ".local" / "share" / "Steam" / "steamapps" / "compatdata" /
                    "489830");
}

TEST_CASE("proton prefix falls back to appmanifest owner", "[engine]") {
  // compatdata not yet created on secondary, but appmanifest is there
  const fs::path root =
      fs::temp_directory_path() / ("gmm_proton_prefix2_" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);
  const fs::path default_root = root / "home" / ".local" / "share" / "Steam";
  const fs::path secondary    = root / "secondary";
  fs::create_directories(default_root / "steamapps");
  fs::create_directories(secondary / "steamapps");
  {
    std::ofstream f(default_root / "steamapps" / "libraryfolders.vdf");
    f << "\"libraryfolders\"\n{\n";
    f << "  \"0\"\n  {\n    \"path\"    \"" << default_root.string() << "\"\n  }\n";
    f << "  \"1\"\n  {\n    \"path\"    \"" << secondary.string() << "\"\n  }\n";
    f << "}\n";
  }
  {
    std::ofstream f(secondary / "steamapps" / "appmanifest_377160.acf");
    f << "\"AppState\" { \"appid\" \"377160\" }\n";
  }
  ::setenv("HOME", (root / "home").c_str(), 1);
  auto p = engine::LinuxPlatform().resolve_proton_prefix(377160);
  REQUIRE(p == secondary / "steamapps" / "compatdata" / "377160");
}

TEST_CASE("proton prefix default-only root works", "[engine]") {
  const fs::path root =
      fs::temp_directory_path() / ("gmm_proton_prefix3_" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);
  const fs::path default_root = root / "home" / ".local" / "share" / "Steam";
  fs::create_directories(default_root / "steamapps" / "compatdata" / "489830");
  {
    std::ofstream f(default_root / "steamapps" / "libraryfolders.vdf");
    f << "\"libraryfolders\"\n{\n";
    f << "  \"0\"\n  {\n    \"path\"    \"" << default_root.string() << "\"\n  }\n";
    f << "}\n";
  }
  ::setenv("HOME", (root / "home").c_str(), 1);
  auto p = engine::LinuxPlatform().resolve_proton_prefix(489830);
  REQUIRE(p == default_root / "steamapps" / "compatdata" / "489830");
  // non-existent appid falls back to default root path
  auto p2 = engine::LinuxPlatform().resolve_proton_prefix(999999);
  REQUIRE(p2 == default_root / "steamapps" / "compatdata" / "999999");
}